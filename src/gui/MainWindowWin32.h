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
  int m_gpPill[4][4];   // GPU power: Auto | Min | Med | Max
  int m_sysTitle[4];    // System Options title row (collapse toggle)
  int m_coTrackX0, m_coTrackX1, m_coTrackY;
  int m_opacityTrackX0, m_opacityTrackX1, m_opacityTrackY;
  int m_btnFlush[4];
  int m_btnCoMinus[4]; // [y0,y1,x0,x1]
  int m_btnCoPlus[4];  // [y0,y1,x0,x1]
  int m_tctlPill[4][4]; // Auto | 95 | 90 | 85
  // [slot][y0,y1,x0,x1]: battery, autostart, minimize, showhud, passive, api,
  // wol, autopower, gameauto
  int m_chk[9][4];

  HWND m_hwnd = nullptr;
  UINT_PTR m_timerId = 0;
  bool m_destroyed = false;
  bool m_draggingSlider = false; // dragging CO or opacity track
  int m_dragTrack = 0;           // 1 = CO, 2 = opacity
  int m_previewCo = 0;           // CO preview during drag (not yet applied)
  int m_flushFeedbackTicks = 0;  // > 0 while showing "Flushed!" feedback
  int m_requiredClientH = 0;     // measured content height (set by OnPaint)
  bool m_autoSized = false;      // window grown to fit content once
  HFONT m_boldFont = nullptr;
  HFONT m_normFont = nullptr;
  HFONT m_smallFont = nullptr;
  // GPU Power hover hint (self-drawn; comctl tooltips unreliable here)
  bool m_hoverGp = false;
  bool m_trackMouse = false;
  int m_gpHoverRect[4] = {0, 0, 0, 0}; // [y0,y1,x0,x1] hit area
  bool m_systemExpanded = false; // System Options card (session-only, starts compact)
};
