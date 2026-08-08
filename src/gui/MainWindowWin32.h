#pragma once

#include <windows.h>

// Native Win32 main window — duplicates the Slint main app UI (Telemetry,
// Power & Fan, AMD PBO, System cards) using GDI. Owns its own telemetry timer
// and handles control clicks (pills, checkboxes, slider, button).
class MainWindowWin32 {
public:
  MainWindowWin32();
  ~MainWindowWin32();

  void Show();
  void Hide();
  void Destroy();
  // Run the native message loop until the window closes (blocks).
  void Run();

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static void RegisterClassOnce();
  void OnPaint(HDC hdc);
  void OnTimer();
  void OnLButtonDown(int x, int y);

  // Layout helpers.
  struct Card { int y, h; };
  Card LayoutCard(int y, const wchar_t *title);

  // Control hit rects (client coords) rebuilt each paint. [y0,y1,x0,x1].
  int m_powerPill[3][4];
  int m_fanPill[4][4];
  int m_muxPill[2][4];
  int m_coTrackX0, m_coTrackX1, m_coTrackY;
  int m_opacityTrackX0, m_opacityTrackX1, m_opacityTrackY;
  int m_btnFlush[4];
  int m_chk[5][4];  // [slot][y0,y1,x0,x1]: battery, autostart, minimize, showhud, passive

  HWND m_hwnd = nullptr;
  UINT_PTR m_timerId = 0;
  bool m_destroyed = false;
  bool m_draggingSlider = false; // dragging CO or opacity track
  int m_dragTrack = 0;           // 1 = CO, 2 = opacity
  int m_previewCo = 0;           // CO preview during drag (not yet applied)
};
