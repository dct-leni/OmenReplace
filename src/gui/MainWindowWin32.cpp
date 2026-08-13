#include "MainWindowWin32.h"

#include <algorithm>
#include <cstdio>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <windowsx.h>

#include "../hal/FanService.h"
#include "../hal/OmenHal.h"
#include "../hal/OmenLog.h"
#include "../hal/PowerControl.h"
#include "HudWindow.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
constexpr int kWidth = 306;
constexpr int kHeight = 735;
constexpr int kCardPad = 8;
constexpr int kRowH = 22;

bool AutoStart() {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    return false;
  bool on = RegQueryValueExW(key, L"OMENControlOptimizer", nullptr, nullptr,
                             nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(key);
  return on;
}

void ToggleAutoStart() {
  HKEY key;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_SET_VALUE, &key) != ERROR_SUCCESS)
    return;
  if (AutoStart()) {
    RegDeleteValueW(key, L"OMENControlOptimizer");
  } else {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
      std::wstring cmd = L"\"";
      cmd += path;
      cmd += L"\"";
      RegSetValueExW(key, L"OMENControlOptimizer", 0, REG_SZ,
                     (const BYTE *)cmd.c_str(),
                     (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    }
  }
  RegCloseKey(key);
}
} // namespace

MainWindowWin32::MainWindowWin32() { RegisterClassOnce(); }

MainWindowWin32::~MainWindowWin32() {
  if (m_boldFont) DeleteObject(m_boldFont);
  if (m_normFont) DeleteObject(m_normFont);
  if (m_smallFont) DeleteObject(m_smallFont);
  if (m_hwnd && !m_destroyed) DestroyWindow(m_hwnd);
}

void MainWindowWin32::RegisterClassOnce() {
  static bool registered = false;
  if (registered) return;
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
  OmenLog("[OMEN] win32 main window show hwnd=%p\n", m_hwnd);
  // Dark title bar (matches the dark theme).
  BOOL dark = TRUE;
  DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd); // force immediate first paint
  m_timerId = SetTimer(m_hwnd, 1, 2000, nullptr);
  m_boldFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
  m_normFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
  m_smallFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
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
    if (self) self->OnPaint(hdc);
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
  case WM_MOUSEMOVE:
    if (self && self->m_draggingSlider)
      self->OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
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
        for (int i = 0; i < 5; i++) {
          if (x >= self->m_chk[i][2] && x <= self->m_chk[i][3] &&
              y >= self->m_chk[i][0] && y <= self->m_chk[i][1]) {
            isInteractive = true; break;
          }
        }
      }
      // Flush button
      if (!isInteractive) {
        if (x >= self->m_btnFlush[2] && x <= self->m_btnFlush[3] &&
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
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

MainWindowWin32::Card MainWindowWin32::LayoutCard(int y,
                                                  const wchar_t *title) {
  // Draw card background + title, return content area bounds.
  return {y + 24, kRowH}; // placeholder; actual drawing in OnPaint
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
  // Checkboxes / Toggle Switches: battery, autostart, minimize, showhud, passive.
  for (int i = 0; i < 5; i++) {
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
      }
      InvalidateRect(m_hwnd, nullptr, FALSE);
      return;
    }
  }
  // Flush button.
  if (x >= m_btnFlush[2] && x <= m_btnFlush[3] && y >= m_btnFlush[0] &&
      y <= m_btnFlush[1]) {
    PowerControl::Get().FlushMemoryWorkingSet();
    OmenHal::Get().OptimizeMemory();
    m_flushFeedbackTicks = 2; // show feedback for 2 update cycles
    InvalidateRect(m_hwnd, nullptr, FALSE);
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

  auto cardBg = [&](int y, int cardH) {
    RECT c = { kCardPad, y, w - kCardPad, y + cardH };
    // Elevated card background: #121218 (RGB 18, 18, 24)
    HBRUSH b = CreateSolidBrush(RGB(18, 18, 24));
    HGDIOBJ old = SelectObject(mem, b);
    RoundRect(mem, c.left, c.top, c.right, c.bottom, 12, 12);
    SelectObject(mem, old);
    DeleteObject(b);

    // Stroke a 1px subtle rounded border: #242430 (RGB 36, 36, 48)
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HPEN p = CreatePen(PS_SOLID, 1, RGB(36, 36, 48));
    old = SelectObject(mem, nb);
    HGDIOBJ oldPen = SelectObject(mem, p);
    RoundRect(mem, c.left, c.top, c.right, c.bottom, 12, 12);
    SelectObject(mem, oldPen);
    SelectObject(mem, old);
    DeleteObject(p);
  };

  auto cardTitle = [&](int y, const wchar_t *t) {
    RECT tr = { kCardPad + 12, y, w - kCardPad - 12, y + 22 };
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
    return RGB(56, 161, 105);               // Green
  };
  auto ramColor = [](float t) {
    if (t > 70) return RGB(230, 50, 50);
    if (t > 55) return RGB(230, 200, 50);
    return RGB(56, 161, 105);
  };
  auto diskColor = [](float t) {
    if (t > 80) return RGB(230, 50, 50);
    if (t > 70) return RGB(230, 200, 50);
    return RGB(56, 161, 105);
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

    // 4px height rounded load bar
    int barY = y + kRowH + 2;
    RECT tr = { kCardPad + 12, barY, w - kCardPad - 12, barY + 4 };
    HBRUSH tb = CreateSolidBrush(RGB(28, 28, 36));
    HGDIOBJ oldTb = SelectObject(mem, tb);
    RoundRect(mem, tr.left, tr.top, tr.right, tr.bottom, 4, 4);
    SelectObject(mem, oldTb);
    DeleteObject(tb);

    float ratio = std::clamp(load / 100.0f, 0.0f, 1.0f);
    int fillW = (int)((float)(tr.right - tr.left) * ratio);
    if (fillW > 0) {
      RECT fr = { tr.left, tr.top, tr.left + fillW, tr.bottom };
      COLORREF barColor = RGB(139, 38, 42);
      if (load > 85.0f) barColor = RGB(230, 50, 50);
      else if (load > 70.0f) barColor = RGB(230, 180, 40);

      HBRUSH fb = CreateSolidBrush(barColor);
      HGDIOBJ oldFb = SelectObject(mem, fb);
      RoundRect(mem, fr.left, fr.top, std::max(fr.left + 4, fr.right), fr.bottom, 4, 4);
      SelectObject(mem, oldFb);
      DeleteObject(fb);
    }
  };

  auto pillRow = [&](int y, int count, const wchar_t *const *labels,
                     int active, int (*rects)[4], int x0, int x1) {
    int gap = 0;
    int pw = (x1 - x0 - (count - 1) * gap) / count;
    int ph = 26;
    // Track #181820, active #8b262a, rounded 8px
    HBRUSH tb = CreateSolidBrush(RGB(24, 24, 32));
    HGDIOBJ oldTb = SelectObject(mem, tb);
    RoundRect(mem, x0, y, x1, y + ph, 8, 8);
    SelectObject(mem, oldTb);
    DeleteObject(tb);
    for (int i = 0; i < count; i++) {
      int px0 = x0 + i * (pw + gap);
      int px1 = px0 + pw;
      rects[i][0] = y;
      rects[i][1] = y + ph;
      rects[i][2] = px0;
      rects[i][3] = px1;
      if (i == active) {
        HBRUSH b = CreateSolidBrush(RGB(139, 38, 42));
        HGDIOBJ old = SelectObject(mem, b);
        RoundRect(mem, px0, y, px1, y + ph, 8, 8);
        SelectObject(mem, old);
        DeleteObject(b);
      }
      SetTextColor(mem, i == active ? RGB(255, 255, 255) : RGB(154, 154, 162));
      SelectObject(mem, m_normFont);
      RECT pr = { px0, y, px1, y + ph };
      DrawTextW(mem, labels[i], -1, &pr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  };

  // Modern Toggle Switch
  auto toggleSwitch = [&](int y, const wchar_t *label, bool on, int slot) {
    int swW = 34, swH = 18;
    int swX = w - kCardPad - 12 - swW;
    int swY = y + (kRowH - swH) / 2;

    // Label on left
    RECT tr = { kCardPad + 12, y, swX - 8, y + kRowH };
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Switch track
    RECT sr = { swX, swY, swX + swW, swY + swH };
    HBRUSH tb = CreateSolidBrush(on ? RGB(139, 38, 42) : RGB(36, 36, 46));
    HGDIOBJ oldTb = SelectObject(mem, tb);
    RoundRect(mem, sr.left, sr.top, sr.right, sr.bottom, 18, 18);
    SelectObject(mem, oldTb);
    DeleteObject(tb);

    // Switch circular thumb (14px diameter)
    int thumbD = 14;
    int thumbX = on ? (swX + swW - thumbD - 2) : (swX + 2);
    int thumbY = swY + 2;
    HBRUSH thb = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ oldThb = SelectObject(mem, thb);
    Ellipse(mem, thumbX, thumbY, thumbX + thumbD, thumbY + thumbD);
    SelectObject(mem, oldThb);
    DeleteObject(thb);

    // Hit box (entire row)
    m_chk[slot][0] = y;
    m_chk[slot][1] = y + kRowH;
    m_chk[slot][2] = kCardPad + 10;
    m_chk[slot][3] = w - kCardPad - 10;
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
  std::swprintf(buf, sizeof(buf), L"%.0f\u00B0C", gpuT);
  metricRow(y, L"GPU", buf, tempColor(gpuT), gpuL);
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
  y += 8;

  // === Card 2: Power & Fan ===
  y += 4;
  int card2y = y;
  cardBg(y, 22 + 30 + 22 + 30 + 22 + 36 + 4);
  y += 4;

  // Power Modes label + pills.
  {
    RECT sr = { kCardPad + 12, y, w - kCardPad - 12, y + 16 };
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, L"Power Modes", -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += 18;
  static const wchar_t *powerLabels[] = {L"Eco", L"Balanced", L"Perf"};
  pillRow(y, 3, powerLabels, hal.GetPowerMode(), m_powerPill, kCardPad + 10,
          w - kCardPad - 10);
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
  y += 36;

  // === Card 3: AMD PBO ===
  y += 4;
  int card3y = y;
  cardBg(y, 22 + 24 + 20);
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
    RECT bmr = { m_btnCoMinus[2], m_btnCoMinus[0], m_btnCoMinus[3], m_btnCoMinus[1] };
    HBRUSH bb = CreateSolidBrush(RGB(28, 28, 38));
    HGDIOBJ oldBb = SelectObject(mem, bb);
    RoundRect(mem, bmr.left, bmr.top, bmr.right, bmr.bottom, 6, 6);
    SelectObject(mem, oldBb);
    DeleteObject(bb);
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
    RECT bpr = { m_btnCoPlus[2], m_btnCoPlus[0], m_btnCoPlus[3], m_btnCoPlus[1] };
    HBRUSH bb = CreateSolidBrush(RGB(28, 28, 38));
    HGDIOBJ oldBb = SelectObject(mem, bb);
    RoundRect(mem, bpr.left, bpr.top, bpr.right, bpr.bottom, 6, 6);
    SelectObject(mem, oldBb);
    DeleteObject(bb);
    SetTextColor(mem, RGB(240, 240, 245));
    SelectObject(mem, m_boldFont);
    DrawTextW(mem, L"+", -1, &bpr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  // Track between buttons
  m_coTrackX0 = kCardPad + 42;
  m_coTrackX1 = w - kCardPad - 42;
  m_coTrackY = y + 10;
  {
    // Track
    RECT tr = { m_coTrackX0, m_coTrackY - 2, m_coTrackX1, m_coTrackY + 2 };
    HBRUSH b = CreateSolidBrush(RGB(28, 28, 36));
    HGDIOBJ oldB = SelectObject(mem, b);
    RoundRect(mem, tr.left, tr.top, tr.right, tr.bottom, 4, 4);
    SelectObject(mem, oldB);
    DeleteObject(b);

    int fillW = (int)((float)(m_coTrackX1 - m_coTrackX0) * (float)(co + 30) /
                      30.0f);
    RECT fr = { m_coTrackX0, m_coTrackY - 2, m_coTrackX0 + fillW,
                m_coTrackY + 2 };
    HBRUSH fb = CreateSolidBrush(RGB(139, 38, 42));
    HGDIOBJ oldFb = SelectObject(mem, fb);
    RoundRect(mem, fr.left, fr.top, std::max(fr.left + 4, fr.right), fr.bottom, 4, 4);
    SelectObject(mem, oldFb);
    DeleteObject(fb);

    // 14px circular thumb handle
    int hx = m_coTrackX0 + fillW;
    HBRUSH ring = CreateSolidBrush(RGB(18, 18, 24));
    HGDIOBJ oldRing = SelectObject(mem, ring);
    Ellipse(mem, hx - 7, m_coTrackY - 7, hx + 7, m_coTrackY + 7);
    SelectObject(mem, oldRing);
    DeleteObject(ring);

    HBRUSH hb = CreateSolidBrush(RGB(139, 38, 42));
    HGDIOBJ oldHb = SelectObject(mem, hb);
    Ellipse(mem, hx - 4, m_coTrackY - 4, hx + 4, m_coTrackY + 4);
    SelectObject(mem, oldHb);
    DeleteObject(hb);
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

  // === Card 4: System ===
  y += 4;
  int card4y = y;
  cardBg(y, 210);
  cardTitle(y, L"System Options");
  y += 26;

  toggleSwitch(y, L"80% Battery Care Mode",
               OmenHal::Get().GetBatteryChargeLimit() <= 80, 0);
  y += kRowH + 4;
  toggleSwitch(y, L"Run on Windows Startup", AutoStart(), 1);
  y += kRowH + 4;
  toggleSwitch(y, L"Minimize to Tray on Close",
               FanService::Get().GetOverlayConfig().minimizeOnClose, 2);
  y += kRowH + 4;

  // Show HUD + HUD Passive on one line
  {
    int half = (w - 2 * kCardPad - 24) / 2;
    auto miniSwitch = [&](int x0, int labelW, const wchar_t *label, bool on, int slot) {
      int swW = 28, swH = 16;
      int swX = x0 + labelW + 4;
      int swY = y + (kRowH - swH) / 2;

      RECT tr = { x0, y, swX - 4, y + kRowH };
      SetTextColor(mem, RGB(235, 235, 240));
      SelectObject(mem, m_smallFont);
      DrawTextW(mem, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

      RECT sr = { swX, swY, swX + swW, swY + swH };
      HBRUSH tb = CreateSolidBrush(on ? RGB(139, 38, 42) : RGB(36, 36, 46));
      HGDIOBJ oldTb = SelectObject(mem, tb);
      RoundRect(mem, sr.left, sr.top, sr.right, sr.bottom, 16, 16);
      SelectObject(mem, oldTb);
      DeleteObject(tb);

      int thumbD = 12;
      int thumbX = on ? (swX + swW - thumbD - 2) : (swX + 2);
      int thumbY = swY + 2;
      HBRUSH thb = CreateSolidBrush(RGB(255, 255, 255));
      HGDIOBJ oldThb = SelectObject(mem, thb);
      Ellipse(mem, thumbX, thumbY, thumbX + thumbD, thumbY + thumbD);
      SelectObject(mem, oldThb);
      DeleteObject(thb);

      m_chk[slot][0] = y;
      m_chk[slot][1] = y + kRowH;
      m_chk[slot][2] = x0;
      m_chk[slot][3] = swX + swW + 4;
    };

    miniSwitch(kCardPad + 12, half - 36, L"Show HUD",
               FanService::Get().GetOverlayConfig().show, 3);
    miniSwitch(kCardPad + 12 + half, half - 36, L"HUD Passive",
               FanService::Get().GetOverlayConfig().hudPassthrough, 4);
  }
  y += kRowH + 2;

  // Opacity slider.
  {
    RECT tr = { kCardPad + 12, y, w - kCardPad - 12, y + kRowH };
    std::swprintf(buf, sizeof(buf), L"HUD Visibility: %d%%",
                  (int)(FanService::Get().GetOverlayConfig().opacity * 100.0f));
    SetTextColor(mem, RGB(235, 235, 240));
    SelectObject(mem, m_normFont);
    DrawTextW(mem, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
  y += kRowH - 4;
  m_opacityTrackX0 = kCardPad + 12;
  m_opacityTrackX1 = w - kCardPad - 12;
  m_opacityTrackY = y + 8;
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
    SelectObject(mem, oldFb);
    DeleteObject(fb);

    int hx = m_opacityTrackX0 + fillW;
    HBRUSH ring = CreateSolidBrush(RGB(18, 18, 24));
    HGDIOBJ oldRing = SelectObject(mem, ring);
    Ellipse(mem, hx - 7, m_opacityTrackY - 7, hx + 7, m_opacityTrackY + 7);
    SelectObject(mem, oldRing);
    DeleteObject(ring);

    HBRUSH hb = CreateSolidBrush(RGB(139, 38, 42));
    HGDIOBJ oldHb = SelectObject(mem, hb);
    Ellipse(mem, hx - 4, m_opacityTrackY - 4, hx + 4, m_opacityTrackY + 4);
    SelectObject(mem, oldHb);
    DeleteObject(hb);
  }
  y += 20;

  // Flush button with feedback animation
  m_btnFlush[0] = y;
  m_btnFlush[1] = y + 28;
  m_btnFlush[2] = kCardPad + 10;
  m_btnFlush[3] = w - kCardPad - 10;
  {
    RECT br = { m_btnFlush[2], m_btnFlush[0], m_btnFlush[3],
                m_btnFlush[1] };
    bool isFlushed = (m_flushFeedbackTicks > 0);
    COLORREF btnBg = isFlushed ? RGB(20, 52, 36) : RGB(28, 28, 38);
    COLORREF btnBorder = isFlushed ? RGB(56, 161, 105) : RGB(52, 52, 66);
    COLORREF btnTxt = isFlushed ? RGB(70, 240, 160) : RGB(235, 235, 240);

    HBRUSH b = CreateSolidBrush(btnBg);
    HBRUSH old = (HBRUSH)SelectObject(mem, b);
    RoundRect(mem, br.left, br.top, br.right, br.bottom, 8, 8);
    SelectObject(mem, old);
    DeleteObject(b);

    SetTextColor(mem, btnTxt);
    SelectObject(mem, m_normFont);
    DrawTextW(mem, isFlushed ? L"\u2713 Flushed RAM Cache" : L"Flush RAM Cache",
              -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ oldN = SelectObject(mem, nb);
    HPEN p = CreatePen(PS_SOLID, 1, btnBorder);
    HGDIOBJ oldP = SelectObject(mem, p);
    RoundRect(mem, br.left, br.top, br.right, br.bottom, 8, 8);
    SelectObject(mem, oldP);
    DeleteObject(p);
    SelectObject(mem, oldN);
  }

  (void)card2y;
  (void)card3y;
  (void)card4y;

  // Present
  BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

  // Restore + cleanup.
  SelectObject(mem, oldBmp);
  DeleteObject(bmp);
  DeleteDC(mem);
}
