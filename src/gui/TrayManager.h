#pragma once

#include <windows.h>

#include <atomic>
#include <thread>

// Native Win32 tray icon. Slint owns the main window/event loop, so the tray
// runs its own hidden window + message pump on a dedicated thread. Tray actions
// invoke HAL methods directly; the Slint UI re-syncs on its 500ms telemetry
// poll.
class TrayManager {
public:
  TrayManager();
  ~TrayManager();

  void Start();
  void Stop();

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam);
  void ThreadMain();
  void HandleMenu(HWND hwnd);
  void SetupIcon(HWND hwnd);

  std::atomic<bool> m_running{false};
  std::thread m_thread;
  HWND m_hwnd = nullptr;
  NOTIFYICONDATAW m_nid = {};
};
