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

  // Control hit rects (client coords) rebuilt each paint. [y0,y1,x0,x1].
  int m_powerPill[4][4];
  int m_fanPill[4][4];
  int m_muxPill[2][4];
  int m_gpPill[3][4];   // GPU power: None | TGP | +Boost
  int m_sysTitle[4];    // System Options title row (collapse toggle)
  int m_coTrackX0, m_coTrackX1, m_coTrackY;
  int m_opacityTrackX0, m_opacityTrackX1, m_opacityTrackY;
  int m_btnFlush[4];
  int m_btnCompact[4];
  int m_btnCoMinus[4]; // [y0,y1,x0,x1]
  int m_btnCoPlus[4];  // [y0,y1,x0,x1]
  int m_tctlPill[4][4]; // Auto | 95 | 90 | 85
  // Switches: battery, autostart, minimize, showhud, passthrough, api,
  // wol, autopower, gameauto, wlanbt
  int m_chk[13][4];

  void UpdateWindowHeight();

  HWND m_hwnd = nullptr;
  UINT_PTR m_timerId = 0;
  bool m_destroyed = false;
  bool m_draggingSlider = false; // dragging CO or opacity track
  int m_dragTrack = 0;           // 1 = CO, 2 = opacity
  int m_previewCo = 0;           // CO preview during drag (not yet applied)
  int m_flushFeedbackTicks = 0;  // > 0 while showing "Flushed!" feedback
  int m_requiredClientH = 0;     // measured content height (set by OnPaint)
  HFONT m_boldFont = nullptr;
  HFONT m_normFont = nullptr;
  HFONT m_smallFont = nullptr;
  // Hover hints (self-drawn floating cards)
  bool m_hoverPower = false;
  int m_powerHoverRect[4] = {0, 0, 0, 0};
  bool m_hoverFan = false;
  int m_fanHoverRect[4] = {0, 0, 0, 0};
  bool m_hoverGp = false;
  int m_gpHoverRect[4] = {0, 0, 0, 0};
  bool m_trackMouse = false;
  bool m_systemExpanded = false; // Collapsible System card
};
