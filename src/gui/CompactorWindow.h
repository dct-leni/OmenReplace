#pragma once

#include <windows.h>
#include <vector>
#include "../hal/CompactorService.h"

class CompactorWindow {
public:
  static CompactorWindow &Instance();

  void Show(HWND hParent = nullptr);
  void Hide();
  void Destroy();

private:
  CompactorWindow();
  ~CompactorWindow();

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static void RegisterClassOnce();
  void ApplyRoundCorners();

  void OnPaint(HDC hdc);
  void OnLButtonDown(int x, int y);
  void OnMouseMove(int x, int y);
  void OnMouseWheel(int delta);
  void OnTimer();
  void UpdateTimerState();
  void OnAddFolder();

  HWND m_hwnd = nullptr;
  HWND m_hParent = nullptr;
  UINT_PTR m_timerId = 0;
  bool m_destroyed = false;

  HFONT m_titleFont = nullptr;
  HFONT m_boldFont = nullptr;
  HFONT m_normFont = nullptr;
  HFONT m_smallFont = nullptr;

  int m_scrollOffset = 0;
  int m_maxScroll = 0;
  float m_spinnerAngle = 0.0f;

  // Hit-test rects
  RECT m_rcClose = {0};
  RECT m_rcCompactAll = {0};
  RECT m_rcRescan = {0};
  RECT m_rcAddFolder = {0};
  RECT m_rcAlgoLzx = {0};
  RECT m_rcAlgoXpress = {0};

  struct GameActionRect {
    size_t gameIndex;
    RECT rcButton;
    bool isDecompact;
    bool isCancel;
  };
  std::vector<GameActionRect> m_gameActionRects;
};
