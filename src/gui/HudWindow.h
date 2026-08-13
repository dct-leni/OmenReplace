#pragma once

#include <windows.h>

// Native Win32 HUD overlay. A real Win32 window (not Slint) so drag/resize,
// opacity, pass-through and taskbar removal all work natively via our own
// WndProc. Slint's message pump dispatches to it since it lives on the same
// UI thread. Drawn with double-buffered GDI.
class HudWindow {
public:
  HudWindow();
  ~HudWindow();

  // Singleton: shared by the Slint UI and the Win32 main window.
  static HudWindow &Instance();

  void Show();
  void Hide();
  void Destroy();
  // Feed telemetry (used by the external telemetry loop).
  void SetTemps(float cpuTemp, float gpuTemp);
  void SetLoads(float cpuLoad, float gpuLoad);
  // Read telemetry directly from the HAL (used by the HUD's own timer).
  void RefreshFromHal();
  // Apply saved position/size from config (posX/posY/sizeW/sizeH). If the
  // saved size is 0, fall back to the default 180x110.
  void ApplySavedPosition(float posX, float posY, float sizeW, float sizeH);
  void SetPassthrough(bool on);
  void SetOpacity(float opacity);
  // Write current window rect into config and persist (called on move/resize end).
  void SavePositionToConfig();

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static void RegisterClassOnce();
  void OnPaint(HDC hdc);
  LRESULT OnNcHitTest(LPARAM lp);
  void ApplyRoundRegion();
  void SnapToScreenBorders(LPRECT rc);

  HWND m_hwnd = nullptr;
  UINT_PTR m_timerId = 0;
  float m_cpuTemp = 0.0f;
  float m_gpuTemp = 0.0f;
  float m_cpuLoad = 0.0f;
  float m_gpuLoad = 0.0f;
  bool m_passthrough = false;
  bool m_styled = false;
};
