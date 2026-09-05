#include "MainWindowWin32.h"

#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cstdio>
#include <dwmapi.h>
#include <string>
#include <thread>
#include <vector>
#include <windowsx.h>

#include "../hal/ApiServer.h"
#include "../hal/FanService.h"
#include "../hal/MemoryService.h"
#include "../hal/OmenHal.h"
#include "../hal/OmenLog.h"
#include "../hal/PowerControl.h"
#include "HudWindow.h"
#include "CompactorWindow.h"

#include <comdef.h>
#include <taskschd.h>

#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "taskschd.lib")

namespace {
constexpr int kWidth = 306;
constexpr int kHeight = 670;
constexpr int kCardPad = 8;
constexpr int kRowH = 22;

// ── Autostart via Windows Task Scheduler COM API (taskschd.h) ────────────────
// A scheduled task with RunLevel=Highest starts the app elevated WITHOUT a
// UAC prompt at logon, and its failure policy restarts it after a crash.
static std::atomic<int> s_autostartCache{-1};
static std::atomic<bool> s_autostartBusy{false};

static bool QueryAutoStartTaskNative() {
  ITaskService *pService = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void **)&pService);
  if (FAILED(hr)) return false;

  hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
  if (FAILED(hr)) {
    pService->Release();
    return false;
  }

  ITaskFolder *pRootFolder = nullptr;
  hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
  if (FAILED(hr)) {
    pService->Release();
    return false;
  }

  IRegisteredTask *pTask = nullptr;
  hr = pRootFolder->GetTask(_bstr_t(L"AMDOMEN"), &pTask);
  bool exists = SUCCEEDED(hr) && (pTask != nullptr);

  if (pTask) pTask->Release();
  pRootFolder->Release();
  pService->Release();
  OmenLog("[AMDOMEN] autostart query native exists=%d\n", exists ? 1 : 0);
  return exists;
}

static bool RegisterAutoStartTaskNative(const wchar_t *exePath) {
  ITaskService *pService = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void **)&pService);
  if (FAILED(hr)) return false;

  hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
  if (FAILED(hr)) { pService->Release(); return false; }

  ITaskFolder *pRootFolder = nullptr;
  hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
  if (FAILED(hr)) { pService->Release(); return false; }

  ITaskDefinition *pTask = nullptr;
  hr = pService->NewTask(0, &pTask);
  if (FAILED(hr)) {
    pRootFolder->Release();
    pService->Release();
    return false;
  }

  // 1. Registration Info
  IRegistrationInfo *pRegInfo = nullptr;
  if (SUCCEEDED(pTask->get_RegistrationInfo(&pRegInfo))) {
    pRegInfo->put_Author(_bstr_t(L"AMDOMEN"));
    pRegInfo->put_Description(_bstr_t(L"AMDOMEN Fan and Power Control Autostart"));
    pRegInfo->Release();
  }

  // 2. Principal: Run with Highest privileges (no UAC prompt at logon)
  IPrincipal *pPrincipal = nullptr;
  if (SUCCEEDED(pTask->get_Principal(&pPrincipal))) {
    pPrincipal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    pPrincipal->Release();
  }

  // 3. Settings: battery support, restart policy, no execution time limit
  ITaskSettings *pSettings = nullptr;
  if (SUCCEEDED(pTask->get_Settings(&pSettings))) {
    pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    pSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    pSettings->put_ExecutionTimeLimit(_bstr_t(L"PT0S")); // Infinite
    pSettings->put_RestartCount(10);
    pSettings->put_RestartInterval(_bstr_t(L"PT1M"));    // 1 minute
    pSettings->Release();
  }

  // 4. Logon Trigger: trigger when user logs on
  ITriggerCollection *pTriggers = nullptr;
  if (SUCCEEDED(pTask->get_Triggers(&pTriggers))) {
    ITrigger *pTrigger = nullptr;
    if (SUCCEEDED(pTriggers->Create(TASK_TRIGGER_LOGON, &pTrigger))) {
      pTrigger->Release();
    }
    pTriggers->Release();
  }

  // 5. Action: launch executable
  IActionCollection *pActions = nullptr;
  if (SUCCEEDED(pTask->get_Actions(&pActions))) {
    IAction *pAction = nullptr;
    if (SUCCEEDED(pActions->Create(TASK_ACTION_EXEC, &pAction))) {
      IExecAction *pExecAction = nullptr;
      if (SUCCEEDED(pAction->QueryInterface(IID_IExecAction, (void **)&pExecAction))) {
        pExecAction->put_Path(_bstr_t(exePath));
        std::wstring dir(exePath);
        size_t sep = dir.find_last_of(L"\\/");
        if (sep != std::wstring::npos) dir = dir.substr(0, sep);
        pExecAction->put_WorkingDirectory(_bstr_t(dir.c_str()));
        pExecAction->Release();
      }
      pAction->Release();
    }
    pActions->Release();
  }

  // 6. Commit registration
  IRegisteredTask *pRegisteredTask = nullptr;
  hr = pRootFolder->RegisterTaskDefinition(
      _bstr_t(L"AMDOMEN"), pTask, TASK_CREATE_OR_UPDATE,
      _variant_t(), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN,
      _variant_t(L""), &pRegisteredTask);

  if (SUCCEEDED(hr) && pRegisteredTask) pRegisteredTask->Release();
  pTask->Release();
  pRootFolder->Release();
  pService->Release();
  OmenLog("[AMDOMEN] autostart register native hr=0x%08lX\n", hr);
  return SUCCEEDED(hr);
}

static bool DeleteAutoStartTaskNative() {
  ITaskService *pService = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void **)&pService);
  if (FAILED(hr)) return false;

  hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
  if (FAILED(hr)) { pService->Release(); return false; }

  ITaskFolder *pRootFolder = nullptr;
  hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
  if (FAILED(hr)) { pService->Release(); return false; }

  hr = pRootFolder->DeleteTask(_bstr_t(L"AMDOMEN"), 0);
  pRootFolder->Release();
  pService->Release();
  OmenLog("[AMDOMEN] autostart delete native hr=0x%08lX\n", hr);
  return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

static bool AutoStart() {
  int cached = s_autostartCache.load();
  if (cached >= 0)
    return cached == 1;
  return false;
}

static void PrimeAutoStartAsync(HWND hwnd) {
  if (s_autostartCache.load() >= 0)
    return;
  std::thread([hwnd]() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool on = QueryAutoStartTaskNative();
    s_autostartCache.store(on ? 1 : 0);
    if (hwnd && IsWindow(hwnd)) {
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    CoUninitialize();
  }).detach();
}

static void ToggleAutoStart() {
  if (s_autostartBusy.exchange(true))
    return; // a toggle is already in flight
  bool turningOn = !AutoStart();
  s_autostartCache.store(turningOn ? 1 : 0); // optimistic, corrected below

  std::thread([turningOn]() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!turningOn) {
      DeleteAutoStartTaskNative();
      s_autostartCache.store(0);
      s_autostartBusy.store(false);
      CoUninitialize();
      return;
    }
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
      s_autostartCache.store(0);
      s_autostartBusy.store(false);
      CoUninitialize();
      return;
    }
    // Migrate away from the legacy registry Run entry, if present.
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
      RegDeleteValueW(key, L"AMDOMEN");
      RegCloseKey(key);
    }

    bool ok = RegisterAutoStartTaskNative(path);
    s_autostartCache.store(ok ? 1 : 0);
    s_autostartBusy.store(false);
    CoUninitialize();
  }).detach();
}
} // namespace

MainWindowWin32::MainWindowWin32() {
  RegisterClassOnce();
  if (!m_boldFont)
    m_boldFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
  if (!m_normFont)
    m_normFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
  if (!m_smallFont)
    m_smallFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
}

MainWindowWin32::~MainWindowWin32() {
  if (m_boldFont) DeleteObject(m_boldFont);
  if (m_normFont) DeleteObject(m_normFont);
  if (m_smallFont) DeleteObject(m_smallFont);
  if (m_hwnd && !m_destroyed) DestroyWindow(m_hwnd);
}

void MainWindowWin32::RegisterClassOnce() {
  static bool registered = false;
  if (registered) return;

  static ULONG_PTR s_gdiplusToken = 0;
  if (!s_gdiplusToken) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&s_gdiplusToken, &gdiplusStartupInput, nullptr);
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(8, 8, 12));
  // omen_icon.ico (resource ID 1) for title bar, taskbar and task manager.
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"AMDOMEN_MAIN_WIN32";
  RegisterClassExW(&wc);
  registered = true;
}

void MainWindowWin32::Show() {
  if (!m_hwnd) {
    m_hwnd = CreateWindowExW(
        0, L"AMDOMEN_MAIN_WIN32", L"AMDOMEN", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return;
  }
  OmenLog("[AMDOMEN] win32 main window show hwnd=%p\n", m_hwnd);
  // Dark title bar (matches the dark theme).
  BOOL dark = TRUE;
  DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd); // force immediate first paint
  if (!m_timerId) m_timerId = SetTimer(m_hwnd, 1, 2000, nullptr);

  // Prime the autostart cache asynchronously in background — never blocks UI thread.
  PrimeAutoStartAsync(m_hwnd);
}

void MainWindowWin32::Hide() { if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE); }

void MainWindowWin32::Destroy() {
  if (m_hwnd && !m_destroyed) {
    if (m_timerId) KillTimer(m_hwnd, m_timerId);
    m_destroyed = true;
    DestroyWindow(m_hwnd);
  }
}

void MainWindowWin32::Run() {
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    if (m_destroyed) break;
  }
}

void MainWindowWin32::UpdateWindowHeight() {
  if (!m_hwnd || m_requiredClientH <= 0) return;
  RECT rc;
  if (GetClientRect(m_hwnd, &rc)) {
    int delta = m_requiredClientH - rc.bottom;
    if (delta != 0) {
      RECT wr;
      if (GetWindowRect(m_hwnd, &wr)) {
        int newW = wr.right - wr.left;
        int newH = wr.bottom - wr.top + delta;
        int newX = wr.left;
        int newY = wr.top;

        HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(hMon, &mi)) {
          if (newY + newH > mi.rcWork.bottom) {
            newY = mi.rcWork.bottom - newH;
          }
          if (newY < mi.rcWork.top) {
            newY = mi.rcWork.top;
          }
        }

        SetWindowPos(m_hwnd, nullptr, newX, newY, newW, newH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
  }
}

LRESULT CALLBACK MainWindowWin32::WndProc(HWND hwnd, UINT msg, WPARAM wp,
                                          LPARAM lp) {
  MainWindowWin32 *self = nullptr;
  if (msg == WM_NCCREATE) {
    auto *cs = (CREATESTRUCTW *)lp;
    self = (MainWindowWin32 *)cs->lpCreateParams;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  } else {
    self = (MainWindowWin32 *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }
  if (self && self->m_hwnd != hwnd) self->m_hwnd = hwnd;

  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (self) {
      self->OnPaint(hdc);
      self->UpdateWindowHeight();
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_TIMER:
    if (self) {
      self->OnTimer();
    }
    return 0;
  case WM_LBUTTONDOWN:
    if (self) self->OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    return 0;
  case WM_MOUSEMOVE: {
    if (self) {
      if (self->m_draggingSlider) {
        self->OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      } else {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        bool insidePower = mx >= self->m_powerHoverRect[2] && mx <= self->m_powerHoverRect[3] &&
                           my >= self->m_powerHoverRect[0] && my <= self->m_powerHoverRect[1];
        bool insideFan = mx >= self->m_fanHoverRect[2] && mx <= self->m_fanHoverRect[3] &&
                         my >= self->m_fanHoverRect[0] && my <= self->m_fanHoverRect[1];
        bool insideGp = mx >= self->m_gpHoverRect[2] && mx <= self->m_gpHoverRect[3] &&
                        my >= self->m_gpHoverRect[0] && my <= self->m_gpHoverRect[1];

        bool changed = false;
        if (insidePower != self->m_hoverPower) { self->m_hoverPower = insidePower; changed = true; }
        if (insideFan != self->m_hoverFan) { self->m_hoverFan = insideFan; changed = true; }
        if (insideGp != self->m_hoverGp) { self->m_hoverGp = insideGp; changed = true; }

        if (changed) {
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        if ((insidePower || insideFan || insideGp) && !self->m_trackMouse) {
          TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
          TrackMouseEvent(&tme);
          self->m_trackMouse = true;
        }
      }
    }
    return 0;
  }
  case WM_MOUSELEAVE:
    if (self) {
      self->m_hoverPower = false;
      self->m_hoverFan = false;
      self->m_hoverGp = false;
      self->m_trackMouse = false;
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
  case WM_LBUTTONUP:
    if (self && self->m_draggingSlider) {
      if (self->m_dragTrack == 1) {
        // Apply the CO value only on release.
        OmenHal::Get().SetAmdCurveOptimizer(self->m_previewCo);
        FanService::Get().SaveConfig();
      }
      self->m_draggingSlider = false;
      self->m_dragTrack = 0;
      ReleaseCapture();
    }
    return 0;
  case WM_SETCURSOR: {
    if (LOWORD(lp) == HTCLIENT && self) {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(hwnd, &pt);
      int x = pt.x, y = pt.y;
      bool isInteractive = false;

      // Power pills
      for (int i = 0; i < 3; i++) {
        if (x >= self->m_powerPill[i][2] && x <= self->m_powerPill[i][3] &&
            y >= self->m_powerPill[i][0] && y <= self->m_powerPill[i][1]) {
          isInteractive = true; break;
        }
      }
      // Fan pills
      if (!isInteractive) {
        for (int i = 0; i < 4; i++) {
          if (x >= self->m_fanPill[i][2] && x <= self->m_fanPill[i][3] &&
              y >= self->m_fanPill[i][0] && y <= self->m_fanPill[i][1]) {
            isInteractive = true; break;
          }
        }
      }
      // GPU MUX pills
      if (!isInteractive) {
        for (int i = 0; i < 2; i++) {
          if (x >= self->m_muxPill[i][2] && x <= self->m_muxPill[i][3] &&
              y >= self->m_muxPill[i][0] && y <= self->m_muxPill[i][1]) {
            isInteractive = true; break;
          }
        }
      }
      // GPU power pills + System collapse title
      if (!isInteractive) {
        for (int i = 0; i < 3 && !isInteractive; i++) {
          if (x >= self->m_gpPill[i][2] && x <= self->m_gpPill[i][3] &&
              y >= self->m_gpPill[i][0] && y <= self->m_gpPill[i][1]) {
            isInteractive = true;
          }
        }
        if (x >= self->m_sysTitle[2] && x <= self->m_sysTitle[3] &&
            y >= self->m_sysTitle[0] && y <= self->m_sysTitle[1]) {
          isInteractive = true;
        }
      }
      // PBO steppers & track
      if (!isInteractive) {
        if ((x >= self->m_btnCoMinus[2] && x <= self->m_btnCoMinus[3] &&
             y >= self->m_btnCoMinus[0] && y <= self->m_btnCoMinus[1]) ||
            (x >= self->m_btnCoPlus[2] && x <= self->m_btnCoPlus[3] &&
             y >= self->m_btnCoPlus[0] && y <= self->m_btnCoPlus[1]) ||
            (x >= self->m_coTrackX0 && x <= self->m_coTrackX1 &&
             y >= self->m_coTrackY - 10 && y <= self->m_coTrackY + 10)) {
          isInteractive = true;
        }
      }
      // Opacity track
      if (!isInteractive) {
        if (x >= self->m_opacityTrackX0 && x <= self->m_opacityTrackX1 &&
            y >= self->m_opacityTrackY - 10 && y <= self->m_opacityTrackY + 10) {
          isInteractive = true;
        }
      }
      // Toggle switches
      if (!isInteractive) {
        for (int i = 0; i < 12; i++) {
          if (x >= self->m_chk[i][2] && x <= self->m_chk[i][3] &&
              y >= self->m_chk[i][0] && y <= self->m_chk[i][1]) {
            isInteractive = true; break;
          }
        }
      }
      // Compact & Flush buttons
      if (!isInteractive) {
        if (x >= self->m_btnCompact[2] && x <= self->m_btnCompact[3] &&
            y >= self->m_btnCompact[0] && y <= self->m_btnCompact[1]) {
          isInteractive = true;
        } else if (x >= self->m_btnFlush[2] && x <= self->m_btnFlush[3] &&
            y >= self->m_btnFlush[0] && y <= self->m_btnFlush[1]) {
          isInteractive = true;
        }
      }

      if (isInteractive) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
    }
    break;
  }
  case WM_APP + 99:
    if (self) {
      if (self->m_timerId) { KillTimer(hwnd, self->m_timerId); self->m_timerId = 0; }
      self->m_destroyed = true;
    }
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    if (self) {
      if (self->m_timerId) { KillTimer(hwnd, self->m_timerId); self->m_timerId = 0; }
      self->m_destroyed = true;
    }
    PostQuitMessage(0);
    return 0;
  case WM_CLOSE:
    if (self && FanService::Get().GetOverlayConfig().minimizeOnClose) {
      self->Hide();
      return 0;
    }
    DestroyWindow(hwnd);
    return 0;
  case WM_QUERYENDSESSION:
    return TRUE;
  case WM_ENDSESSION:
    if (wp) {
      OmenHal::Get().Shutdown();
    }
    return 0;
  case WM_DISPLAYCHANGE:
    if (self) {
      HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi = {sizeof(mi)};
      if (GetMonitorInfoW(hMon, &mi)) {
        RECT wr;
        if (GetWindowRect(hwnd, &wr)) {
          if (wr.left >= mi.rcWork.right || wr.right <= mi.rcWork.left ||
              wr.top >= mi.rcWork.bottom || wr.bottom <= mi.rcWork.top) {
            SetWindowPos(hwnd, nullptr, mi.rcWork.left + 50, mi.rcWork.top + 50,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
          }
        }
      }
      InvalidateRect(hwnd, nullptr, FALSE);
    }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void MainWindowWin32::OnTimer() {
  if (m_flushFeedbackTicks > 0) {
    m_flushFeedbackTicks--;
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindowWin32::OnLButtonDown(int x, int y) {
  // Power mode pills.
  for (int i = 0; i < 3; i++) {
    if (x >= m_powerPill[i][2] && x <= m_powerPill[i][3] &&
        y >= m_powerPill[i][0] && y <= m_powerPill[i][1]) {
      OmenHal::Get().SetPowerMode(i);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }
  // Fan profile pills.
  for (int i = 0; i < 4; i++) {
    if (x >= m_fanPill[i][2] && x <= m_fanPill[i][3] &&
        y >= m_fanPill[i][0] && y <= m_fanPill[i][1]) {
      if (i == 0)
        FanService::Get().SetFanAuto();
      else {
        FanService::Get().SetControlMode(FanControlMode::AppMode);
        FanService::Get().SetProfile((FanControlProfile)(i - 1));
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }
  // GPU MUX pills.
  for (int i = 0; i < 2; i++) {
    if (x >= m_muxPill[i][2] && x <= m_muxPill[i][3] &&
        y >= m_muxPill[i][0] && y <= m_muxPill[i][1]) {
      OmenHal::Get().RequestGpuMode(i);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }

  // GPU Power pills: None | TGP | +Boost.
  for (int i = 0; i < 3; i++) {
    if (x >= m_gpPill[i][2] && x <= m_gpPill[i][3] &&
        y >= m_gpPill[i][0] && y <= m_gpPill[i][1]) {
      PowerControl::Get().SetGpuPowerOverride(i);
      PowerControl::Get().SetGpuPower((uint8_t)i);
      auto &cfg = FanService::Get().GetOverlayConfig();
      cfg.gpuPowerLevel = i;
      FanService::Get().SaveConfig();
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }

  // System Options title: collapse/expand
  if (x >= m_sysTitle[2] && x <= m_sysTitle[3] && y >= m_sysTitle[0] &&
      y <= m_sysTitle[1]) {
    m_systemExpanded = !m_systemExpanded;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // AMD CO [-] / [+] Stepper Buttons
  // Tctl limit pills.
  for (int i = 0; i < 4; i++) {
    if (x >= m_tctlPill[i][2] && x <= m_tctlPill[i][3] &&
        y >= m_tctlPill[i][0] && y <= m_tctlPill[i][1]) {
      static const int vals[4] = {0, 95, 90, 85};
      int v = vals[i];
      auto &cfg = FanService::Get().GetOverlayConfig();
      cfg.tctlLimit = v;
      FanService::Get().SaveConfig();
      if (v > 0)
        OmenHal::Get().SetTctlTemp(v);
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }

  // AMD CO [-] / [+] Stepper Buttons
  if (x >= m_btnCoMinus[2] && x <= m_btnCoMinus[3] &&
      y >= m_btnCoMinus[0] && y <= m_btnCoMinus[1]) {
    int co = OmenHal::Get().GetCachedAmdCurveOptimizer();
    int val = std::clamp(co - 1, -30, 0);
    OmenHal::Get().SetAmdCurveOptimizer(val);
    FanService::Get().SaveConfig();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  if (x >= m_btnCoPlus[2] && x <= m_btnCoPlus[3] &&
      y >= m_btnCoPlus[0] && y <= m_btnCoPlus[1]) {
    int co = OmenHal::Get().GetCachedAmdCurveOptimizer();
    int val = std::clamp(co + 1, -30, 0);
    OmenHal::Get().SetAmdCurveOptimizer(val);
    FanService::Get().SaveConfig();
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  // AMD CO track: preview while dragging, apply on mouse release.
  if (x >= m_coTrackX0 && x <= m_coTrackX1 && y >= m_coTrackY - 8 &&
      y <= m_coTrackY + 8) {
    int val = -30 + (int)((float)(x - m_coTrackX0) /
                          (float)(m_coTrackX1 - m_coTrackX0) * 30.0f);
    if (!m_draggingSlider) {
      // Click-to-position: apply immediately.
      OmenHal::Get().SetAmdCurveOptimizer(val);
      FanService::Get().SaveConfig();
      m_draggingSlider = true;
      m_dragTrack = 1;
      m_previewCo = val;
      SetCapture(m_hwnd);
    } else if (m_dragTrack == 1) {
      // Dragging: update preview only.
      m_previewCo = val;
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  // Opacity track.
  if (x >= m_opacityTrackX0 && x <= m_opacityTrackX1 &&
      y >= m_opacityTrackY - 6 && y <= m_opacityTrackY + 6) {
    int val = 10 + (int)((float)(x - m_opacityTrackX0) /
                         (float)(m_opacityTrackX1 - m_opacityTrackX0) * 90.0f);
    float op = (float)val / 100.0f;
    auto &cfg = FanService::Get().GetOverlayConfig();
    cfg.opacity = op;
    FanService::Get().SaveConfig();
    HudWindow::Instance().SetOpacity(op);
    if (!m_draggingSlider) {
      m_draggingSlider = true;
      m_dragTrack = 2;
      SetCapture(m_hwnd);
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  // Checkboxes / Toggle Switches: battery, autostart, minimize, showhud,
  // passive, api, wol, autopower, gameauto, overdrive, wlanbt, netgaming.
  for (int i = 0; i < 12; i++) {
    if (x >= m_chk[i][2] && x <= m_chk[i][3] && y >= m_chk[i][0] &&
        y <= m_chk[i][1]) {
      switch (i) {
      case 0:
        OmenHal::Get().SetBatteryChargeLimit(
            OmenHal::Get().GetBatteryChargeLimit() <= 80 ? 100 : 80);
        break;
      case 1:
        ToggleAutoStart();
        break;
      case 2: {
        auto &cfg = FanService::Get().GetOverlayConfig();
        cfg.minimizeOnClose = !cfg.minimizeOnClose;
        FanService::Get().SaveConfig();
        break;
      }
      case 3: {
        auto &cfg = FanService::Get().GetOverlayConfig();
        if (cfg.show) {
          HudWindow::Instance().Hide();
          cfg.show = false;
        } else {
          HudWindow::Instance().Show();
          HudWindow::Instance().ApplySavedPosition(cfg.posX, cfg.posY, cfg.sizeW, cfg.sizeH);
          HudWindow::Instance().SetOpacity(cfg.opacity);
          HudWindow::Instance().SetPassthrough(cfg.hudPassthrough);
          cfg.show = true;
        }
        FanService::Get().SaveConfig();
        break;
      }
      case 4: {
        auto &cfg = FanService::Get().GetOverlayConfig();
        cfg.hudPassthrough = !cfg.hudPassthrough;
        HudWindow::Instance().SetPassthrough(cfg.hudPassthrough);
        FanService::Get().SaveConfig();
        break;
      }
      case 5: {
        auto &cfg = FanService::Get().GetOverlayConfig();
        cfg.apiEnabled = !cfg.apiEnabled;
        FanService::Get().SaveConfig();
        if (cfg.apiEnabled)
          ApiServer::Get().Start();
        else
          ApiServer::Get().Stop();
        break;
      }
      case 6: {
        // Wake-on-LAN — toggle real NIC state + BIOS S3/S4/S5.
        bool cur = PowerControl::Get().GetWakeOnLan();
        PowerControl::Get().SetWakeOnLan(!cur);
        break;
      }
      case 7: {
        auto &cfg = FanService::Get().GetOverlayConfig();
        cfg.autoPowerSwitch = !cfg.autoPowerSwitch;
        PowerControl::Get().SetAcEnabled(cfg.autoPowerSwitch);
        FanService::Get().SaveConfig();
        break;
      }
      case 8: {
        auto &cfg = FanService::Get().GetOverlayConfig();
        cfg.gameAutoProfile = !cfg.gameAutoProfile;
        FanService::Get().SaveConfig();
        break;
      }
      case 9: {
        bool cur = OmenHal::Get().GetDisplayOverdrive();
        OmenHal::Get().SetDisplayOverdrive(!cur);
        break;
      }
      case 10: {
        // Wake on WLAN/BT — toggle Wireless & Bluetooth wake.
        bool cur = PowerControl::Get().GetWakeOnWlanBt();
        PowerControl::Get().SetWakeOnWlanBt(!cur);
        break;
      }
      case 11: {
        // Low-Latency Network & OS Gaming Tweak
        bool cur = PowerControl::Get().GetNetworkGamingTweak();
        PowerControl::Get().SetNetworkGamingTweak(!cur);
        break;
      }
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }
  // Compact Games button.
  if (x >= m_btnCompact[2] && x <= m_btnCompact[3] && y >= m_btnCompact[0] &&
      y <= m_btnCompact[1]) {
    CompactorWindow::Instance().Show(m_hwnd);
    return;
  }
  // Flush button.
  if (x >= m_btnFlush[2] && x <= m_btnFlush[3] && y >= m_btnFlush[0] &&
      y <= m_btnFlush[1]) {
    PowerControl::Get().FlushMemoryWorkingSet();
    std::thread([] {
      OmenHal::Get().OptimizeMemory();
    }).detach();
    m_flushFeedbackTicks = 2; // show feedback for 2 update cycles
    InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }
  InvalidateRect(m_hwnd, nullptr, FALSE);
}

void MainWindowWin32::OnPaint(HDC hdc) {
  RECT rc;
  GetClientRect(m_hwnd, &rc);
  int w = rc.right, h = rc.bottom;

  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
  HGDIOBJ oldBmp = SelectObject(mem, bmp);

  // Background: Deep Obsidian #08080c (RGB 8, 8, 12)
  HBRUSH bg = CreateSolidBrush(RGB(8, 8, 12));
  FillRect(mem, &rc, bg);
  DeleteObject(bg);

  SetBkMode(mem, TRANSPARENT);

  // High-performance single GDI+ context per frame (avoids 20+ re-instantiations)
  Gdiplus::Graphics gfx(mem);
  gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

  auto fillRoundRect = [&](float x, float y, float w, float h, float r,
                          const Gdiplus::Brush &brush, const Gdiplus::Pen *pen = nullptr) {
    if (w <= 0 || h <= 0) return;
    Gdiplus::GraphicsPath path;
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    gfx.FillPath(&brush, &path);
    if (pen) {
      gfx.DrawPath(pen, &path);
    }
  };

  auto cardBg = [&](int y, int cardH) {
    float cx = (float)kCardPad;
    float cy = (float)y;
    float cw = (float)(w - 2 * kCardPad);
    float ch = (float)cardH;

    Gdiplus::SolidBrush cardBrush(Gdiplus::Color(255, 18, 18, 24));
    Gdiplus::Pen borderPen(Gdiplus::Color(255, 36, 36, 48), 1.0f);
    fillRoundRect(cx, cy, cw, ch, 10.0f, cardBrush, &borderPen);
  };

  auto cardTitle = [&](int y, const wchar_t *t) {
    // 3px inset from the card's top edge.
    RECT tr = { kCardPad + 12, y + 3, w - kCardPad - 12, y + 21 };
    SelectObject(mem, m_normFont);
    SetTextColor(mem, RGB(240, 240, 245));
    DrawTextW(mem, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  };

  auto &hal = OmenHal::Get();
  float cpuT = hal.GetCpuTemp(), gpuT = hal.GetGpuTemp();
  if (gpuT > 120.0f) gpuT = 0.0f;
  float cpuL = hal.GetCpuLoad(), gpuL = hal.GetGpuLoad();
  float ramT = hal.GetRamTemp();
  float ramTemp0 = 0, ramTemp1 = 0;
  int ramDimms = hal.GetRamTemps(ramTemp0, ramTemp1);
  float fan1 = hal.GetFanSpeed(0), fan2 = hal.GetFanSpeed(1);
  float cpuP = hal.GetCpuPower(), gpuP = hal.GetGpuPower();
  float total = hal.GetTotalPower();
  float cpuVolts = PowerControl::Get().GetCpuVoltage();
  float ramUsed = 0, ramTotal = 0, ramPct = 0;
  PowerControl::Get().GetSystemRamUsage(ramUsed, ramTotal, ramPct);

  auto tempColor = [](float t) {
    if (t > 85) return RGB(230, 50, 50);    // Red
    if (t > 75) return RGB(230, 200, 50);   // Amber
    return RGB(50, 240, 180);               // Green
  };
  auto ramColor = [](float t) {
    if (t > 70) return RGB(230, 50, 50);
    if (t > 55) return RGB(230, 200, 50);
    return RGB(50, 240, 180);
  };
  auto diskColor = [](float t) {
    if (t > 80) return RGB(230, 50, 50);
    if (t > 70) return RGB(230, 200, 50);
    return RGB(50, 240, 180);
  };

  auto metricRow = [&](int y, const wchar_t *label, const wchar_t *value,
                       COLORREF vc, float load) {
    RECT lr = { kCardPad + 12, y, kCardPad + 110, y + kRowH };
    SelectObject(mem, m_normFont);
    SetTextColor(mem, RGB(235, 235, 240));
    DrawTextW(mem, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT vr = { kCardPad + 110, y, w - kCardPad - 12, y + kRowH };
    SetTextColor(mem, vc);
    DrawTextW(mem, value, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 4px height rounded load bar with GDI+ anti-aliasing
    int barY = y + kRowH + 2;
    float bx = (float)(kCardPad + 12);
    float by = (float)barY;
    float bw = (float)(w - 2 * kCardPad - 24);
    float bh = 4.0f;

    Gdiplus::SolidBrush trackBrush(Gdiplus::Color(255, 28, 28, 36));
    fillRoundRect(bx, by, bw, bh, 2.0f, trackBrush);

    float ratio = std::clamp(load / 100.0f, 0.0f, 1.0f);
    float fillW = bw * ratio;
    if (fillW > 2.0f) {
      Gdiplus::Color barColor(255, 139, 38, 42);
      if (load > 85.0f) barColor = Gdiplus::Color(255, 230, 50, 50);
      else if (load > 70.0f) barColor = Gdiplus::Color(255, 230, 180, 40);

      Gdiplus::SolidBrush fillBrush(barColor);
      fillRoundRect(bx, by, fillW, bh, 2.0f, fillBrush);
    }
  };

  auto pillRow = [&](int y, int count, const wchar_t *const *labels,
                     int active, int (*rects)[4], int x0, int x1) {
    int gap = 2;
    int totalW = x1 - x0;
    int pw = (totalW - (count - 1) * gap) / count;
    int ph = 26;

    // Track background (#181820 / RGB 24, 24, 32)
    Gdiplus::SolidBrush trackBrush(Gdiplus::Color(255, 24, 24, 32));
    Gdiplus::Pen trackPen(Gdiplus::Color(255, 34, 34, 44), 1.0f);
    fillRoundRect((float)x0, (float)y, (float)totalW, (float)ph, 8.0f, trackBrush, &trackPen);

    for (int i = 0; i < count; i++) {
      int px0 = x0 + i * (pw + gap);
      int px1 = px0 + pw;
      rects[i][0] = y;
      rects[i][1] = y + ph;
      rects[i][2] = px0;
      rects[i][3] = px1;

      if (i == active) {
        Gdiplus::SolidBrush activeBrush(Gdiplus::Color(255, 139, 38, 42));
        Gdiplus::Pen activePen(Gdiplus::Color(255, 180, 52, 58), 1.0f);
        fillRoundRect((float)px0 + 1.0f, (float)y + 1.0f, (float)pw - 2.0f, (float)ph - 2.0f,
                      6.0f, activeBrush, &activePen);
      }

      SetTextColor(mem, i == active ? RGB(255, 255, 255) : RGB(154, 154, 162));
      SelectObject(mem, m_normFont);
      RECT pr = { px0, y, px1, y + ph };
      DrawTextW(mem, labels[i], -1, &pr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  };

  // === Header: power on top, CPU/GPU names below ===
  int y = 8;
  {
    wchar_t powerBuf[64];
    std::swprintf(powerBuf, sizeof(powerBuf),
                  L"C:%.0fW | G:%.0fW | %.0fW", cpuP, gpuP,
                  total > 0 ? total : 0.0f);
    RECT hr = { kCardPad, y, w - kCardPad, y + 14 };
    SetTextColor(mem, RGB(240, 240, 245));
    SelectObject(mem, m_smallFont);
    DrawTextW(mem, powerBuf, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 16;
  {
    wchar_t hwBuf[160];
    std::swprintf(hwBuf, sizeof(hwBuf), L"%hs", hal.GetCpuName().c_str());
    wchar_t gpuBuf[160];
    std::swprintf(gpuBuf, sizeof(gpuBuf), L" / %hs",
                  hal.GetGpuName().c_str());
    wcscat_s(hwBuf, gpuBuf);
    RECT hr = { kCardPad, y, w - kCardPad, y + 13 };
    SetTextColor(mem, RGB(154, 154, 162));
    SelectObject(mem, m_smallFont);
    DrawTextW(mem, hwBuf, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 16;

  // Count disks up front so card heights can be computed correctly.
  auto drives = hal.GetDriveTemps();
  int diskCount = (int)std::min<size_t>(drives.size(), 3);

  // === Card 1: Telemetry ===
  int card1y = y;
  int telemetryH = 26 + 28 + 28 + 28 + 34 + diskCount * 28 + 8;
  cardBg(y, telemetryH);
  cardTitle(y, L"Telemetry");
  y += 26;
  wchar_t buf[64];

  std::swprintf(buf, sizeof(buf), L"%.0f\u00B0C", cpuT);
  wchar_t cpubuf[32];
  std::swprintf(cpubuf, sizeof(cpubuf), L"CPU (%.2fV)", cpuVolts);
  metricRow(y, cpubuf, buf, tempColor(cpuT), cpuL);
  y += kRowH + 6;
  if (gpuT > 0.0f) {
    std::swprintf(buf, sizeof(buf), L"%.0f\u00B0C", gpuT);
    metricRow(y, L"GPU", buf, tempColor(gpuT), gpuL);
  } else {
    metricRow(y, L"GPU", L"Sleep (0\u00B0C)", RGB(140, 140, 158), 0.0f);
  }
  y += kRowH + 6;
  if (ramDimms > 0 && ramT > 0 && ramT < 100) {
    if (ramDimms >= 2) {
      std::swprintf(buf, sizeof(buf), L"%.0f/%.0f\u00B0C", ramTemp0, ramTemp1);
    } else {
      std::swprintf(buf, sizeof(buf), L"%.0f\u00B0C", ramT);
    }
  } else {
    wcscpy_s(buf, L"\u2014");
  }
  metricRow(y, L"RAM", buf, ramColor(ramT), ramPct);
  y += kRowH + 6;
  auto rpmfn = [](float v) { return (int)((int)(v / 100.0f) * 100); };
  std::swprintf(buf, sizeof(buf), L"%d / %d RPM", rpmfn(fan1), rpmfn(fan2));
  metricRow(y, L"Fans", buf, RGB(235, 235, 240), 0);
  y += kRowH + 12;

  // Disks.
  for (size_t i = 0; i < drives.size() && i < 3; i++) {
    std::string model = drives[i].Model;
    auto strip = [&model](const std::string &word) {
      size_t p = model.find(word);
      while (p != std::string::npos) {
        model.erase(p, word.size());
        while (p < model.size() && model[p] == ' ') model.erase(p, 1);
        p = model.find(word);
      }
    };
    strip("NVMe");
    strip("NVME");
    strip("PC");
    while (!model.empty() && model[0] == ' ') model.erase(0, 1);
    std::string label = model + " [" + std::to_string(drives[i].Health) + "%]";
    std::wstring wlabel(label.begin(), label.end());
    int labelEnd = w - kCardPad - 12 - 80;
    RECT lr = { kCardPad + 12, y, labelEnd, y + kRowH };
    SelectObject(mem, m_normFont);
    SetTextColor(mem, RGB(235, 235, 240));
    DrawTextW(mem, wlabel.c_str(), -1, &lr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    wchar_t tbuf[16];
    std::swprintf(tbuf, sizeof(tbuf), L" %.0f\u00B0C", (float)drives[i].Temperature);
    RECT vr = { labelEnd, y, w - kCardPad - 12, y + kRowH };
    SetTextColor(mem, diskColor((float)drives[i].Temperature));
    DrawTextW(mem, tbuf, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    y += kRowH + 6;
  }

  // === Card 2: Power & Fan ===
  y = card1y + telemetryH + 4;
  int card2y = y;
  cardBg(y, 204); // 4 pill rows; GPU Power row has a hover tooltip
  y += 4;

  // Power Modes label + pills.
  {
    RECT sr = { kCardPad + 12, y, w - kCardPad - 12, y + 16 };
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, L"Power Modes", -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 18;
  static const wchar_t *powerLabels[] = {L"Eco", L"Balanced", L"Performance"};
  pillRow(y, 3, powerLabels, hal.GetPowerMode(), m_powerPill, kCardPad + 10,
          w - kCardPad - 10);
  m_powerHoverRect[0] = y - 18;
  m_powerHoverRect[1] = y + 28;
  m_powerHoverRect[2] = kCardPad + 10;
  m_powerHoverRect[3] = w - kCardPad - 10;
  y += 30;

  // Fan Profiles label + pills.
  {
    RECT sr = { kCardPad + 12, y, w - kCardPad - 12, y + 16 };
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, L"Fan Profiles", -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 18;
  static const wchar_t *fanLabels[] = {L"BIOS", L"Default", L"Quiet", L"Cool"};
  int fanIdx = (int)FanService::Get().GetControlMode() == 0
                   ? 0
                   : 1 + (int)FanService::Get().GetProfile();
  pillRow(y, 4, fanLabels, fanIdx, m_fanPill, kCardPad + 10, w - kCardPad - 10);
  m_fanHoverRect[0] = y - 18;
  m_fanHoverRect[1] = y + 28;
  m_fanHoverRect[2] = kCardPad + 10;
  m_fanHoverRect[3] = w - kCardPad - 10;
  y += 30;

  // GPU MUX label + pills.
  {
    RECT sr = { kCardPad + 12, y, w - kCardPad - 12, y + 16 };
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, L"GPU MUX", -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 18;
  static const wchar_t *muxLabels[] = {L"Hybrid", L"Discrete"};
  pillRow(y, 2, muxLabels, hal.GetGpuModeInt(), m_muxPill, kCardPad + 10,
          w - kCardPad - 10);
  y += 30;

  // GPU Power (TGP) label + pills: None | TGP | +Boost.
  {
    RECT sr = { kCardPad + 12, y, w - kCardPad - 12, y + 16 };
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, L"GPU Power", -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 18;
  {
    static const wchar_t *gpLabels[] = {L"None", L"TGP", L"+Boost"};
    int gpIdx = PowerControl::Get().GetEffectiveGpuPowerLevel();
    if (gpIdx < 0) gpIdx = 0;
    if (gpIdx > 2) gpIdx = 2;
    pillRow(y, 3, gpLabels, gpIdx, m_gpPill, kCardPad + 10,
            w - kCardPad - 10);
    // Hover-hint hit area covers label + pills.
    m_gpHoverRect[0] = y - 18;
    m_gpHoverRect[1] = y + 28;
    m_gpHoverRect[2] = kCardPad + 10;
    m_gpHoverRect[3] = w - kCardPad - 10;
  }

  // === Card 3: AMD PBO ===
  y = card2y + 204 + 4;
  int card3y = y;
  cardBg(y, 118);
  cardTitle(y, L"AMD PBO");
  y += 24;
  int co = hal.GetCachedAmdCurveOptimizer();
  if (m_draggingSlider && m_dragTrack == 1) co = m_previewCo;
  if (co > 0) co = 0;
  if (co < -30) co = -30;

  // [-] Stepper button on left
  m_btnCoMinus[0] = y;
  m_btnCoMinus[1] = y + 20;
  m_btnCoMinus[2] = kCardPad + 12;
  m_btnCoMinus[3] = kCardPad + 12 + 22;
  {
    Gdiplus::SolidBrush btnBrush(Gdiplus::Color(255, 28, 28, 38));
    Gdiplus::Pen btnPen(Gdiplus::Color(255, 48, 48, 60), 1.0f);
    fillRoundRect((float)m_btnCoMinus[2], (float)m_btnCoMinus[0], 22.0f, 20.0f, 5.0f, btnBrush, &btnPen);

    RECT bmr = { m_btnCoMinus[2], m_btnCoMinus[0], m_btnCoMinus[3], m_btnCoMinus[1] };
    SetTextColor(mem, RGB(240, 240, 245));
    SelectObject(mem, m_boldFont);
    DrawTextW(mem, L"-", -1, &bmr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  // [+] Stepper button on right
  m_btnCoPlus[0] = y;
  m_btnCoPlus[1] = y + 20;
  m_btnCoPlus[2] = w - kCardPad - 12 - 22;
  m_btnCoPlus[3] = w - kCardPad - 12;
  {
    Gdiplus::SolidBrush btnBrush(Gdiplus::Color(255, 28, 28, 38));
    Gdiplus::Pen btnPen(Gdiplus::Color(255, 48, 48, 60), 1.0f);
    fillRoundRect((float)m_btnCoPlus[2], (float)m_btnCoPlus[0], 22.0f, 20.0f, 5.0f, btnBrush, &btnPen);

    RECT bpr = { m_btnCoPlus[2], m_btnCoPlus[0], m_btnCoPlus[3], m_btnCoPlus[1] };
    SetTextColor(mem, RGB(240, 240, 245));
    SelectObject(mem, m_boldFont);
    DrawTextW(mem, L"+", -1, &bpr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  // Track between buttons
  m_coTrackX0 = kCardPad + 42;
  m_coTrackX1 = w - kCardPad - 42;
  m_coTrackY = y + 10;
  {
    float tx = (float)m_coTrackX0;
    float ty = (float)(m_coTrackY - 2);
    float tw = (float)(m_coTrackX1 - m_coTrackX0);
    float th = 4.0f;

    Gdiplus::SolidBrush trackBrush(Gdiplus::Color(255, 28, 28, 36));
    fillRoundRect(tx, ty, tw, th, 2.0f, trackBrush);

    float fillW = tw * (float)(co + 30) / 30.0f;
    if (fillW > 2.0f) {
      Gdiplus::SolidBrush fillBrush(Gdiplus::Color(255, 139, 38, 42));
      fillRoundRect(tx, ty, fillW, th, 2.0f, fillBrush);
    }

    // 14px circular thumb handle
    float hx = tx + fillW;
    float cy = (float)m_coTrackY;

    Gdiplus::SolidBrush ringBrush(Gdiplus::Color(255, 18, 18, 24));
    gfx.FillEllipse(&ringBrush, hx - 7.0f, cy - 7.0f, 14.0f, 14.0f);

    Gdiplus::SolidBrush hbBrush(Gdiplus::Color(255, 139, 38, 42));
    gfx.FillEllipse(&hbBrush, hx - 4.0f, cy - 4.0f, 8.0f, 8.0f);

    Gdiplus::Pen knobPen(Gdiplus::Color(255, 200, 60, 66), 1.0f);
    gfx.DrawEllipse(&knobPen, hx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
  }
  y += 22;
  {
    RECT rr = { kCardPad + 10, y, w - kCardPad - 10, y + 18 };
    std::swprintf(buf, sizeof(buf), L"Set: %d counts", co);
    SetTextColor(mem, RGB(154, 154, 162));
    SelectObject(mem, m_smallFont);
    DrawTextW(mem, buf, -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  y += 20;

  // CPU Temp Limit (Tctl) pill row.
  {
    RECT sr = { kCardPad + 12, y, w - kCardPad - 12, y + 16 };
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, L"CPU Temp Limit", -1, &sr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 18;
  {
    int tctl = FanService::Get().GetOverlayConfig().tctlLimit;
    static const wchar_t *tctlLabels[] = {L"Auto", L"95\u00B0", L"90\u00B0",
                                          L"85\u00B0"};
    int tctlIdx = tctl == 95 ? 1 : tctl == 90 ? 2 : tctl == 85 ? 3 : 0;
    pillRow(y, 4, tctlLabels, tctlIdx, m_tctlPill, kCardPad + 12,
            w - kCardPad - 12);
  }

  // === Card 4: System (collapsible) ===
  y = card3y + 118 + 4;
  int card4y = y;
  bool sysExpanded = m_systemExpanded;
  cardBg(y, sysExpanded ? 404 : 32);

  // Clickable title row with chevron.
  m_sysTitle[0] = y;
  m_sysTitle[1] = y + 26;
  m_sysTitle[2] = kCardPad + 10;
  m_sysTitle[3] = w - kCardPad - 10;
  cardTitle(y, L"System Options");
  {
    RECT cr = { w - kCardPad - 30, y, w - kCardPad - 10, y + 22 };
    SetTextColor(mem, RGB(154, 154, 162));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, sysExpanded ? L"\u25B4" : L"\u25BE", -1, &cr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 26;

    if (!sysExpanded) {
    // Invalidate all toggle hit-rects while collapsed.
    for (int i = 0; i < 11; i++) {
      m_chk[i][0] = m_chk[i][1] = 0;
    }
    m_btnCompact[0] = m_btnCompact[1] = 0;
    m_btnFlush[0] = m_btnFlush[1] = 0;
    m_requiredClientH = card4y + 32 + 12;
  } else {
    auto sectionHeader = [&](const wchar_t *title) {
      RECT hr = { kCardPad + 12, y + 2, w - kCardPad - 12, y + 16 };
      SetTextColor(mem, RGB(140, 140, 158));
      SelectObject(mem, m_smallFont);
      DrawTextW(mem, title, -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      y += 18;
    };

    auto singleRowSwitch = [&](const wchar_t *label, bool on, int slot) {
      int swW = 32, swH = 16;
      int x0 = kCardPad + 12;
      int x1 = w - kCardPad - 12;
      int swX = x1 - swW;
      int swY = y + (24 - swH) / 2;

      RECT tr = { x0, y, swX - 8, y + 24 };
      SetTextColor(mem, RGB(235, 235, 240));
      SelectObject(mem, m_normFont);
      DrawTextW(mem, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

      // Smooth capsule track
      Gdiplus::GraphicsPath path;
      float r = (float)swH;
      float fx = (float)swX, fy = (float)swY, fw = (float)swW;
      path.AddArc(fx, fy, r, r, 90.0f, 180.0f);
      path.AddArc(fx + fw - r, fy, r, r, 270.0f, 180.0f);
      path.CloseFigure();

      Gdiplus::Color trackColor = on ? Gdiplus::Color(255, 139, 38, 42) : Gdiplus::Color(255, 34, 34, 44);
      Gdiplus::SolidBrush trackBrush(trackColor);
      gfx.FillPath(&trackBrush, &path);

      Gdiplus::Pen trackPen(on ? Gdiplus::Color(255, 175, 48, 54) : Gdiplus::Color(255, 48, 48, 60), 1.0f);
      gfx.DrawPath(&trackPen, &path);

      // Smooth circular thumb
      float thumbD = (float)swH - 4.0f; // 12px
      float thumbX = on ? (fx + fw - thumbD - 2.0f) : (fx + 2.0f);
      float thumbY = fy + 2.0f;
      Gdiplus::SolidBrush thumbBrush(Gdiplus::Color(255, 255, 255, 255));
      gfx.FillEllipse(&thumbBrush, thumbX, thumbY, thumbD, thumbD);

      m_chk[slot][0] = y;
      m_chk[slot][1] = y + 24;
      m_chk[slot][2] = x0;
      m_chk[slot][3] = x1;

      y += 24;
    };

    // --- Hardware & Power Section ---
    sectionHeader(L"HARDWARE & POWER");
    singleRowSwitch(L"80% Battery Charge Limit", OmenHal::Get().GetBatteryChargeLimit() <= 80, 0);
    singleRowSwitch(L"Display Panel Overdrive", OmenHal::Get().GetDisplayOverdrive(), 9);
    singleRowSwitch(L"Wake on LAN (Ethernet S4/S5)", PowerControl::Get().GetWakeOnLan(), 6);
    singleRowSwitch(L"Wake on WLAN & Bluetooth", PowerControl::Get().GetWakeOnWlanBt(), 10);
    singleRowSwitch(L"Auto Power on AC / Battery", FanService::Get().GetOverlayConfig().autoPowerSwitch, 7);
    singleRowSwitch(L"Low-Latency Network Tweak", PowerControl::Get().GetNetworkGamingTweak(), 11);

    y += 4;

    // --- Application & Overlay Section ---
    sectionHeader(L"APPLICATION & HUD");
    singleRowSwitch(L"Start with Windows", AutoStart(), 1);
    singleRowSwitch(L"Minimize to System Tray", FanService::Get().GetOverlayConfig().minimizeOnClose, 2);
    singleRowSwitch(L"Fullscreen Game Profile", FanService::Get().GetOverlayConfig().gameAutoProfile, 8);
    singleRowSwitch(L"Performance HUD Overlay", FanService::Get().GetOverlayConfig().show, 3);
    singleRowSwitch(L"HUD Click Passthrough", FanService::Get().GetOverlayConfig().hudPassthrough, 4);
    singleRowSwitch(L"Local REST API Server", FanService::Get().GetOverlayConfig().apiEnabled, 5);

    y += 6;

    // Opacity slider
    {
      RECT tr = { kCardPad + 12, y, w - kCardPad - 12, y + 16 };
      std::swprintf(buf, sizeof(buf), L"HUD Visibility: %d%%",
                    (int)(FanService::Get().GetOverlayConfig().opacity * 100.0f));
      SetTextColor(mem, RGB(235, 235, 240));
      SelectObject(mem, m_normFont);
      DrawTextW(mem, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y += 14;
    m_opacityTrackX0 = kCardPad + 12;
    m_opacityTrackX1 = w - kCardPad - 12;
    m_opacityTrackY = y + 6;
    {
      int op = (int)(FanService::Get().GetOverlayConfig().opacity * 100.0f);
      RECT tr = { m_opacityTrackX0, m_opacityTrackY - 2, m_opacityTrackX1,
                  m_opacityTrackY + 2 };
      HBRUSH b = CreateSolidBrush(RGB(28, 28, 36));
      HGDIOBJ oldB = SelectObject(mem, b);
      RoundRect(mem, tr.left, tr.top, tr.right, tr.bottom, 4, 4);
      SelectObject(mem, oldB);
      DeleteObject(b);

      int fillW = (int)((float)(m_opacityTrackX1 - m_opacityTrackX0) *
                        (float)(op - 10) / 90.0f);
      RECT fr = { m_opacityTrackX0, m_opacityTrackY - 2, m_opacityTrackX0 + fillW,
                  m_opacityTrackY + 2 };
      HBRUSH fb = CreateSolidBrush(RGB(139, 38, 42));
      HGDIOBJ oldFb = SelectObject(mem, fb);
      RoundRect(mem, fr.left, fr.top, std::max(fr.left + 4, fr.right), fr.bottom, 4, 4);
      
      float hx = (float)(m_opacityTrackX0 + fillW);
      float cy = (float)m_opacityTrackY;

      Gdiplus::SolidBrush ringBrush(Gdiplus::Color(255, 18, 18, 24));
      gfx.FillEllipse(&ringBrush, hx - 6.0f, cy - 6.0f, 12.0f, 12.0f);

      Gdiplus::SolidBrush hbBrush(Gdiplus::Color(255, 139, 38, 42));
      gfx.FillEllipse(&hbBrush, hx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
      
      SelectObject(mem, oldFb);
      DeleteObject(fb);
    }
    y += 24;

    // Compact Games button
    m_btnCompact[0] = y;
    m_btnCompact[1] = y + 26;
    m_btnCompact[2] = kCardPad + 10;
    m_btnCompact[3] = w - kCardPad - 10;
    {
      float bx = (float)m_btnCompact[2];
      float by = (float)m_btnCompact[0];
      float bw = (float)(m_btnCompact[3] - m_btnCompact[2]);
      float bh = (float)(m_btnCompact[1] - m_btnCompact[0]);

      Gdiplus::Color btnBg = Gdiplus::Color(255, 28, 28, 38);
      Gdiplus::Color btnBorder = Gdiplus::Color(255, 52, 52, 66);
      COLORREF btnTxt = RGB(235, 235, 240);

      Gdiplus::SolidBrush btnBrush(btnBg);
      Gdiplus::Pen btnPen(btnBorder, 1.0f);
      fillRoundRect(bx, by, bw, bh, 6.0f, btnBrush, &btnPen);

      RECT br = { m_btnCompact[2], m_btnCompact[0], m_btnCompact[3], m_btnCompact[1] };
      SetTextColor(mem, btnTxt);
      SelectObject(mem, m_normFont);
      DrawTextW(mem, L"Compact Games...", -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    y += 32;

    // Flush button with feedback animation
    m_btnFlush[0] = y;
    m_btnFlush[1] = y + 26;
    m_btnFlush[2] = kCardPad + 10;
    m_btnFlush[3] = w - kCardPad - 10;
    {
      float bx = (float)m_btnFlush[2];
      float by = (float)m_btnFlush[0];
      float bw = (float)(m_btnFlush[3] - m_btnFlush[2]);
      float bh = (float)(m_btnFlush[1] - m_btnFlush[0]);

      bool isFlushed = (m_flushFeedbackTicks > 0);
      Gdiplus::Color btnBg = isFlushed ? Gdiplus::Color(255, 20, 52, 36) : Gdiplus::Color(255, 28, 28, 38);
      Gdiplus::Color btnBorder = isFlushed ? Gdiplus::Color(255, 50, 240, 180) : Gdiplus::Color(255, 52, 52, 66);
      COLORREF btnTxt = isFlushed ? RGB(50, 240, 180) : RGB(235, 235, 240);

      Gdiplus::SolidBrush btnBrush(btnBg);
      Gdiplus::Pen btnPen(btnBorder, 1.0f);
      fillRoundRect(bx, by, bw, bh, 6.0f, btnBrush, &btnPen);

      RECT br = { m_btnFlush[2], m_btnFlush[0], m_btnFlush[3], m_btnFlush[1] };
      SetTextColor(mem, btnTxt);
      SelectObject(mem, m_normFont);
      DrawTextW(mem, isFlushed ? L"\u2713 Flushed RAM Cache" : L"Flush RAM Cache",
                -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    m_requiredClientH = m_btnFlush[1] + 16;
  }

  (void)card2y;
  (void)card3y;
  (void)card4y;

  // Hover hint panels — drawn last so they float on top.
  auto drawHintBox = [&](int rect[4], const wchar_t *title, const wchar_t *lines[], int count) {
    if (rect[1] <= rect[0]) return;
    int bx0 = rect[2];
    int bx1 = rect[3];
    int boxH = (count + 1) * 16 + 12;
    int by0 = rect[1] + 6;
    if (by0 + boxH > h - 10) by0 = rect[0] - boxH - 6;
    int by1 = by0 + boxH;

    float fx = (float)bx0, fy = (float)by0, fw = (float)(bx1 - bx0), fh = (float)boxH;

    Gdiplus::SolidBrush boxBrush(Gdiplus::Color(245, 22, 22, 30));
    Gdiplus::Pen boxPen(Gdiplus::Color(255, 60, 60, 74), 1.0f);
    fillRoundRect(fx, fy, fw, fh, 8.0f, boxBrush, &boxPen);

    int ly = by0 + 7;
    RECT tr = {bx0 + 12, ly, bx1 - 12, ly + 16};
    SetTextColor(mem, RGB(240, 240, 245));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ly += 16;

    SelectObject(mem, m_smallFont);
    SetTextColor(mem, RGB(180, 180, 190));
    for (int i = 0; i < count; i++) {
      RECT lr = {bx0 + 12, ly, bx1 - 12, ly + 16};
      DrawTextW(mem, lines[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      ly += 16;
    }
  };

  if (m_hoverPower && m_powerHoverRect[1] > m_powerHoverRect[0]) {
    const wchar_t *lines[] = {
        L"Eco: 25W CPU / Base GPU (Quiet battery)",
        L"Balanced: 45W CPU / Standard TGP (Daily)",
        L"Performance: 60W Sweet-Spot CPU / Max 140W GPU"};
    drawHintBox(m_powerHoverRect, L"Power Modes (CPU/GPU limits)", lines, 3);
  } else if (m_hoverFan && m_fanHoverRect[1] > m_fanHoverRect[0]) {
    const wchar_t *lines[] = {
        L"BIOS: Factory internal ACPI fan tables",
        L"Default: Quiet idle, progressive curve",
        L"Quiet: Low acoustics (fans <=50% to 80\u00B0C)",
        L"Cool: Max cooling (100% boost at >=75\u00B0C)"};
    drawHintBox(m_fanHoverRect, L"Fan Profiles (Curves & Speeds)", lines, 4);
  } else if (m_hoverGp && m_gpHoverRect[1] > m_gpHoverRect[0]) {
    const wchar_t *lines[] = {
        L"None: Base TGP limit (no boost)",
        L"TGP: Standard GPU power cap",
        L"+Boost: TGP + NVIDIA Dynamic Boost"};
    drawHintBox(m_gpHoverRect, L"GPU Power", lines, 3);
  }

  // Present
  BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

  // Restore + cleanup.
  SelectObject(mem, oldBmp);
  DeleteObject(bmp);
  DeleteDC(mem);
}
