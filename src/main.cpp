#include <d3d11.h>
#include <iostream>
#include <string>
#include <tchar.h>
#include <vector>
#include <windows.h>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"

// HAL and GUI
#include "gui/Overlay.h"
#include "hal/OmenHal.h"

// Data
static ID3D11Device *g_pd3dDevice = NULL;
static ID3D11DeviceContext *g_pd3dDeviceContext = NULL;
static IDXGISwapChain *g_pSwapChain = NULL;
static ID3D11RenderTargetView *g_mainRenderTargetView = NULL;

// Force iGPU usage to prevent dGPU wake-up (Thermal Optimization)
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000000;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0;
}

// Global overlay pointer for WndProc access
static Overlay *g_pOverlay = nullptr;

#define WM_TRAYICON (WM_USER + 1)

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <shellapi.h>

bool IsUserAdmin() {
  BOOL bIsAdmin = FALSE;
  PSID AdministratorsGroup = NULL;
  SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
  if (AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                               DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                               &AdministratorsGroup)) {
    CheckTokenMembership(NULL, AdministratorsGroup, &bIsAdmin);
    FreeSid(AdministratorsGroup);
  }
  return bIsAdmin;
}

// Main code
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow) {
  // Ensure Admin permissions
  if (!IsUserAdmin()) {
    char szPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szPath, ARRAYSIZE(szPath))) {
      SHELLEXECUTEINFOA sei = {sizeof(sei)};
      sei.cbSize = sizeof(sei);
      sei.lpVerb = "runas";
      sei.lpFile = szPath;
      sei.hwnd = NULL;
      sei.nShow = SW_NORMAL;
      if (ShellExecuteExA(&sei)) {
        return 0;
      }
    }
    return 1;
  }

  // Initialize COM Security for WMI
  HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
  if (SUCCEEDED(hres)) {
    hres = CoInitializeSecurity(
        NULL,
        -1,                          // COM authentication
        NULL,                        // Authentication services
        NULL,                        // Reserved
        RPC_C_AUTHN_LEVEL_DEFAULT,   // Default authentication
        RPC_C_IMP_LEVEL_IMPERSONATE, // Default Impersonation
        NULL,                        // Authentication info
        EOAC_NONE,                   // Additional capabilities
        NULL                         // Reserved
    );
  }

  // Create application window
  ImGui_ImplWin32_EnableDpiAwareness();
  WNDCLASSEXW wc = {sizeof(wc),
                    CS_CLASSDC,
                    WndProc,
                    0L,
                    0L,
                    GetModuleHandle(NULL),
                    LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1)),
                    LoadCursor(NULL, IDC_ARROW),
                    NULL,
                    NULL,
                    L"Omen Control Tool",
                    LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1))};
  ::RegisterClassExW(&wc);

  // Compact window: fixed width, height adjusts dynamically to content
  int WIN_W = 340;
  int WIN_H = 560; // Initial height; ImGui sizes window to content each frame
  RECT wr = {0, 0, WIN_W, WIN_H};
  DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX;
  AdjustWindowRect(&wr, dwStyle, FALSE);

  HWND hwnd = ::CreateWindowW(
      wc.lpszClassName, L"Omen Control Tool", dwStyle | WS_VISIBLE, 100, 100,
      wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, wc.hInstance, NULL);

  // Initialize Direct3D
  if (!CreateDeviceD3D(hwnd)) {
    CleanupDeviceD3D();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  // Show the window
  ::ShowWindow(hwnd, SW_SHOWDEFAULT);
  ::UpdateWindow(hwnd);

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport

  // Load Sharp Font
  ImFontConfig fontConfig;
  fontConfig.RasterizerMultiply = 1.2f;

  // Glyph range: Basic Latin + Latin-1 Supplement (includes ° at U+00B0)
  static const ImWchar latinRanges[] = {0x0020, 0x00FF, 0};

  char fontPath[MAX_PATH];
  GetWindowsDirectoryA(fontPath, MAX_PATH);

  // Try system fonts in order; all include the Latin-1 Supplement
  const char *candidates[] = {"segoeui.ttf", "arial.ttf", "verdana.ttf",
                              nullptr};
  bool fontLoaded = false;
  for (int i = 0; candidates[i] && !fontLoaded; ++i) {
    std::string path = std::string(fontPath) + "\\Fonts\\" + candidates[i];
    fontLoaded = io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, &fontConfig,
                                              latinRanges) != nullptr;
  }
  if (!fontLoaded)
    io.Fonts->AddFontDefault(); // ProggyClean fallback (ASCII only)

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();

  ImGuiStyle &style = ImGui::GetStyle();
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.0f;
  }

  // Setup Platform/Renderer backends
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  // Initialize Omen HAL
  if (!OmenHal::Get().Initialize()) {
  }

  Overlay overlay;
  overlay.SetHwnd(hwnd);
  g_pOverlay = &overlay;

  ImVec4 clear_color = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);

  // Main loop
  bool done = false;
  while (!done) {
    MSG msg;
    while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        done = true;
    }
    if (done)
      break;

    // Skip rendering main window when minimized to tray (save CPU)
    // ONLY if the overlay is disabled AND ImGui has successfully closed its
    // viewports.
    if (overlay.IsTrayMode() && !overlay.IsOverlayVisible() &&
        ImGui::GetPlatformIO().Viewports.Size == 1) {
      // Wait for up to 500ms, but wake instantly if a tray message arrives
      MsgWaitForMultipleObjects(0, NULL, FALSE, 500, QS_ALLINPUT);
      continue;
    }

    // Force cap frame rate to ~30 FPS to massively reduce CPU usage (0.8% ->
    // 0.1%) This applies whether the app is open or if it's minimized with
    // overlay active
    static DWORD lastTime = 0;
    DWORD currentTime = GetTickCount();
    if (currentTime - lastTime < 33) {
      Sleep(33 - (currentTime - lastTime));
      currentTime = GetTickCount();
    }
    lastTime = currentTime;

    // Start the Dear ImGui frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Check if we need to skip Present (DXGI warns if we present on minimized
    // window)
    bool skipPresent = IsIconic(hwnd);

    // Render Overlay
    overlay.Render(OmenHal::Get());

    // Rendering
    ImGui::Render();
    if (!skipPresent) {
      const float clear_color_with_alpha[4] = {
          clear_color.x * clear_color.w, clear_color.y * clear_color.w,
          clear_color.z * clear_color.w, clear_color.w};
      g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
      g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                                 clear_color_with_alpha);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
      g_pSwapChain->Present(1, 0); // Present with vsync
    }

    // Update and Render additional Platform Windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
    }
  }

  // Cleanup
  overlay.RemoveTrayIcon();
  g_pOverlay = nullptr;

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  OmenHal::Get().Shutdown();
  ImGui::DestroyContext();

  CleanupDeviceD3D();
  ::DestroyWindow(hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

  return 0;
}

// Helper functions
bool CreateDeviceD3D(HWND hWnd) {
  // Setup swap chain
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT createDeviceFlags = 0;

  // Explicitly find Integrated GPU (Non-NVIDIA)
  IDXGIAdapter *pAdapter = NULL;
  IDXGIFactory *pFactory = NULL;
  if (CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&pFactory) == S_OK) {
    for (UINT i = 0;
         pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      DXGI_ADAPTER_DESC desc;
      pAdapter->GetDesc(&desc);
      if (desc.VendorId != 0x10DE) {
        break;
      }
      pAdapter->Release();
      pAdapter = NULL;
    }
    pFactory->Release();
  }

  D3D_DRIVER_TYPE driverType =
      pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray[2] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };
  HRESULT res = D3D11CreateDeviceAndSwapChain(
      pAdapter, driverType, NULL, createDeviceFlags, featureLevelArray, 2,
      D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
      &g_pd3dDeviceContext);

  if (pAdapter)
    pAdapter->Release();

  if (res != S_OK)
    return false;

  CreateRenderTarget();
  return true;
}

void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = NULL;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = NULL;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = NULL;
  }
}

void CreateRenderTarget() {
  ID3D11Texture2D *pBackBuffer;
  g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
  g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL,
                                       &g_mainRenderTargetView);
  pBackBuffer->Release();
}

void CleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = NULL;
  }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) {
  case WM_SIZE:
    if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
      CleanupRenderTarget();
      g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                  DXGI_FORMAT_UNKNOWN, 0);
      CreateRenderTarget();
    }
    // Minimize to tray
    if (wParam == SIZE_MINIMIZED && g_pOverlay) {
      g_pOverlay->SetTrayMode(true);
      g_pOverlay->SetupTrayIcon();
      ShowWindow(hWnd, SW_HIDE);
    }
    return 0;

  case WM_TRAYICON:
    if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
      if (g_pOverlay) {
        g_pOverlay->RestoreFromTray();
      }
    } else if (lParam == WM_RBUTTONUP && g_pOverlay) {
      g_pOverlay->HandleTrayMenu();
    }
    return 0;

  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU)
      return 0;
    break;
  case WM_DESTROY:
    ::PostQuitMessage(0);
    return 0;
  }
  return ::DefWindowProc(hWnd, msg, wParam, lParam);
}
