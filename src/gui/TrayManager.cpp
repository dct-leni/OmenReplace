#include "TrayManager.h"

#include "../hal/FanService.h"
#include "../hal/OmenHal.h"
#include "../hal/PowerControl.h"
#include "../hal/OmenLog.h"
#include <windows.h>

#define WM_TRAYICON (WM_USER + 1)

#define IDM_TRAY_RESTORE 1001
#define IDM_TRAY_EXIT 1003
#define IDM_TRAY_FAN_AUTO 1004
#define IDM_TRAY_FAN_APP 1005
#define IDM_TRAY_FAN_PROFILE_QUIET 1006
#define IDM_TRAY_FAN_PROFILE_COOL 1007
#define IDM_TRAY_FAN_PROFILE_DEFAULT 1008
#define IDM_TRAY_PM_ECO 1009
#define IDM_TRAY_PM_BALANCED 1010
#define IDM_TRAY_PM_PERF 1011
#define IDM_TRAY_PM_TURBO 1012

static void SetFanAuto() { FanService::Get().SetFanAuto(); }

static void SetFanApp(int profile) {
  FanService::Get().SetControlMode(FanControlMode::AppMode);
  FanService::Get().SetProfile((FanControlProfile)profile);
}

TrayManager::TrayManager() {}

TrayManager::~TrayManager() { Stop(); }

void TrayManager::Start() {
  m_running = true;
  m_thread = std::thread(&TrayManager::ThreadMain, this);
}

void TrayManager::Stop() {
  m_running = false;
  if (m_thread.joinable()) {
    if (m_hwnd)
      PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    m_thread.join();
  }
}

void TrayManager::ThreadMain() {
  const wchar_t kClassName[] = L"AMDOMENTrayWnd";
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = TrayManager::WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  m_hwnd = CreateWindowExW(0, kClassName, L"AMDOMEN Tray", WS_POPUP, 0, 0, 1,
                           1, nullptr, nullptr, wc.hInstance, this);
  if (!m_hwnd) {
    UnregisterClassW(kClassName, wc.hInstance);
    return;
  }

  SetupIcon(m_hwnd);

  MSG msg;
  while (m_running && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  Shell_NotifyIconW(NIM_DELETE, &m_nid);
  DestroyWindow(m_hwnd);
  UnregisterClassW(kClassName, wc.hInstance);
  m_hwnd = nullptr;
}

static UINT s_uTaskbarRestart = 0;

void TrayManager::SetupIcon(HWND hwnd) {
  if (!s_uTaskbarRestart)
    s_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");

  // Allow lower-integrity processes (explorer.exe) to send TaskbarCreated & tray clicks
  // when AMDOMEN is running elevated under Task Scheduler (TASK_RUNLEVEL_HIGHEST).
  ChangeWindowMessageFilter(s_uTaskbarRestart, MSGFLT_ADD);
  ChangeWindowMessageFilter(WM_TRAYICON, MSGFLT_ADD);
  ChangeWindowMessageFilter(WM_COMMAND, MSGFLT_ADD);
  ChangeWindowMessageFilterEx(hwnd, s_uTaskbarRestart, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(hwnd, WM_TRAYICON, MSGFLT_ALLOW, nullptr);
  ChangeWindowMessageFilterEx(hwnd, WM_COMMAND, MSGFLT_ALLOW, nullptr);

  ZeroMemory(&m_nid, sizeof(m_nid));
  m_nid.cbSize = sizeof(m_nid);
  m_nid.hWnd = hwnd;
  m_nid.uID = 1;
  m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  m_nid.uCallbackMessage = WM_TRAYICON;
  m_nid.hIcon =
      LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
  wcscpy_s(m_nid.szTip, L"AMDOMEN");
  // Delete any existing icon first (in case of taskbar reload/reset)
  Shell_NotifyIconW(NIM_DELETE, &m_nid);
  BOOL added = Shell_NotifyIconW(NIM_ADD, &m_nid);
  if (!added) {
    // If explorer.exe is still initializing during logon, retry every 1.5s until added
    SetTimer(hwnd, 101, 1500, nullptr);
  } else {
    KillTimer(hwnd, 101);
  }
}

LRESULT CALLBACK TrayManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {
  TrayManager *self = (TrayManager *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (msg == WM_NCCREATE) {
    CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
    self = (TrayManager *)cs->lpCreateParams;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    if (!s_uTaskbarRestart)
      s_uTaskbarRestart = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilter(s_uTaskbarRestart, MSGFLT_ADD);
    ChangeWindowMessageFilter(WM_TRAYICON, MSGFLT_ADD);
    ChangeWindowMessageFilter(WM_COMMAND, MSGFLT_ADD);
    ChangeWindowMessageFilterEx(hwnd, s_uTaskbarRestart, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_TRAYICON, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COMMAND, MSGFLT_ALLOW, nullptr);
    return TRUE;
  }
  if (msg == WM_TIMER && wParam == 101) {
    if (self) self->SetupIcon(hwnd);
    return 0;
  }
  if (s_uTaskbarRestart != 0 && msg == s_uTaskbarRestart) {
    if (self) self->SetupIcon(hwnd);
    return 0;
  }
  if (msg == WM_TRAYICON) {
    UINT evt = (UINT)LOWORD(lParam);
    if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU || lParam == WM_RBUTTONUP) {
      if (self) self->HandleMenu(hwnd);
    } else if (evt == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONDBLCLK) {
      if (self) {
        // Restore the main window (show it and bring to front).
        HWND main = FindWindowW(L"AMDOMEN_MAIN_WIN32", nullptr);
        if (main) {
          ShowWindow(main, SW_RESTORE);
          SetForegroundWindow(main);
        }
      }
    }
    return 0;
  }
  if (msg == WM_CLOSE) {
    DestroyWindow(hwnd);
    return 0;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void TrayManager::HandleMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, IDM_TRAY_RESTORE, L"Show Window");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  bool appMode =
      FanService::Get().GetControlMode() == FanControlMode::AppMode;
  int profile = (int)FanService::Get().GetProfile();

  HMENU fanMenu = CreatePopupMenu();
  AppendMenuW(fanMenu, MF_STRING | (appMode ? MF_UNCHECKED : MF_CHECKED),
              IDM_TRAY_FAN_AUTO, L"BIOS Auto");
  AppendMenuW(fanMenu, MF_STRING | (appMode && profile == 0 ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_FAN_PROFILE_DEFAULT, L"App: Default");
  AppendMenuW(fanMenu, MF_STRING | (appMode && profile == 1 ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_FAN_PROFILE_QUIET, L"App: Quiet");
  AppendMenuW(fanMenu, MF_STRING | (appMode && profile == 2 ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_FAN_PROFILE_COOL, L"App: Cool");
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)fanMenu, L"Fan Control");

  int pm = OmenHal::Get().GetPowerMode();
  HMENU pmMenu = CreatePopupMenu();
  AppendMenuW(pmMenu, MF_STRING | (pm == 0 ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_PM_ECO, L"Eco");
  AppendMenuW(pmMenu, MF_STRING | (pm == 1 ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_PM_BALANCED, L"Balanced");
  AppendMenuW(pmMenu, MF_STRING | (pm == 2 ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_PM_PERF, L"Performance");
  AppendMenuW(pmMenu, MF_STRING | (pm == 3 ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_PM_TURBO, L"Turbo");
  AppendMenuW(menu, MF_POPUP, (UINT_PTR)pmMenu, L"Power Mode");

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                           hwnd, nullptr);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
  DestroyMenu(fanMenu);
  DestroyMenu(pmMenu);

  switch (cmd) {
  case IDM_TRAY_RESTORE: {
    HWND main = FindWindowW(L"AMDOMEN_MAIN_WIN32", nullptr);
    if (main) {
      ShowWindow(main, SW_RESTORE);
      SetForegroundWindow(main);
    }
    break;
  }
  case IDM_TRAY_FAN_AUTO:
    SetFanAuto();
    break;
  case IDM_TRAY_FAN_PROFILE_DEFAULT:
    SetFanApp(0);
    break;
  case IDM_TRAY_FAN_PROFILE_QUIET:
    SetFanApp(1);
    break;
  case IDM_TRAY_FAN_PROFILE_COOL:
    SetFanApp(2);
    break;
  case IDM_TRAY_PM_ECO:
    OmenHal::Get().SetPowerMode(0);
    break;
  case IDM_TRAY_PM_BALANCED:
    OmenHal::Get().SetPowerMode(1);
    break;
  case IDM_TRAY_PM_PERF:
    OmenHal::Get().SetPowerMode(2);
    break;
  case IDM_TRAY_PM_TURBO:
    OmenHal::Get().SetPowerMode(3);
    break;
  case IDM_TRAY_EXIT: {
    HWND main = FindWindowW(L"AMDOMEN_MAIN_WIN32", nullptr);
    if (main) {
      PostMessageW(main, WM_APP + 99, 0, 0);
    } else {
      HWND hud = FindWindowW(L"AMDOMEN_HUD_WINDOW", nullptr);
      if (hud) PostMessageW(hud, WM_CLOSE, 0, 0);
      PostQuitMessage(0);
    }
    break;
  }
  }
}
