#pragma once
#include "../hal/OmenHal.h"
#include "imgui.h"
#include <windows.h>

class Overlay {
public:
  Overlay();
  ~Overlay();

  void Render(OmenHal &hal);
  bool IsEditorVisible() const { return m_showEditor; }
  bool IsOverlayVisible() const { return m_showOverlayHUD; }

  // AutoStart
  bool IsAutoStartEnabled() const;
  void SetAutoStart(bool enable);

  // Tray icon
  void SetHwnd(HWND hwnd) { m_hwnd = hwnd; }
  void SetupTrayIcon();
  void RemoveTrayIcon();
  void UpdateTrayIcon(float cpuTemp, float gpuTemp);
  bool IsTrayMode() const { return m_trayMode; }
  void SetTrayMode(bool v) { m_trayMode = v; }
  void RestoreFromTray();
  void HandleTrayMenu();

private:
  void SyncConfig();

  // Fan curve editor
  int m_draggingIdx;
  const char *m_draggingId;
  bool m_showEditor;

  // Popups
  bool m_showOptions;

  // Overlay HUD
  bool m_showOverlayHUD;
  float m_hudOpacity;
  ImVec2 m_hudSize;
  bool m_hudResizable;
  bool m_hudAlwaysOnTop;
  bool m_hudVertical;
  ImVec2 m_hudPos;

  // Tray
  HWND m_hwnd;
  NOTIFYICONDATAW m_nid;
  bool m_trayMode;
  HICON m_trayIcon;
};
