#include "HudWindow.h"

#include "../hal/FanService.h"
#include "../hal/OmenHal.h"
#include "../hal/PowerControl.h"
#include <algorithm>
#include <dwmapi.h>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")

namespace {
constexpr int kWidth = 180;
constexpr int kHeight = 135;
constexpr int kResizeGrip = 16;
} // namespace

HudWindow::HudWindow() { RegisterClassOnce(); }

HudWindow &HudWindow::Instance() {
  static HudWindow instance;
  return instance;
}

HudWindow::~HudWindow() {
  if (m_hwnd) DestroyWindow(m_hwnd);
}

void HudWindow::RegisterClassOnce() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(14, 14, 18));
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = L"AMDOMEN_HUD_WINDOW";
  RegisterClassExW(&wc);
  registered = true;
}

void HudWindow::Show() {
  if (!m_hwnd) {
    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"AMDOMEN_HUD_WINDOW", L"AMDOMEN HUD", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return;
  }
  ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
  SetOpacity(FanService::Get().GetOverlayConfig().opacity);
  ApplyRoundRegion();
  RefreshFromHal();
  if (!m_timerId) m_timerId = SetTimer(m_hwnd, 2, 1000, nullptr);
}

void HudWindow::ApplyRoundRegion() {
  if (!m_hwnd) return;
  RECT rc;
  GetClientRect(m_hwnd, &rc);
  HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, 8, 8);
  SetWindowRgn(m_hwnd, rgn, TRUE); // system owns the region now
}

void HudWindow::Hide() {
  if (!m_hwnd) return;
  if (m_timerId) { KillTimer(m_hwnd, m_timerId); m_timerId = 0; }
  ShowWindow(m_hwnd, SW_HIDE);
}

void HudWindow::Destroy() {
  if (!m_hwnd) return;
  if (m_timerId) { KillTimer(m_hwnd, m_timerId); m_timerId = 0; }
  DestroyWindow(m_hwnd);
  m_hwnd = nullptr;
}

void HudWindow::SetTemps(float cpuTemp, float gpuTemp) {
  m_cpuTemp = cpuTemp;
  m_gpuTemp = gpuTemp;
  if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HudWindow::SetLoads(float cpuLoad, float gpuLoad) {
  m_cpuLoad = cpuLoad;
  m_gpuLoad = gpuLoad;
  if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HudWindow::RefreshFromHal() {
  float cpuT = OmenHal::Get().GetCpuTemp();
  float gpuT = OmenHal::Get().GetGpuTemp();
  if (gpuT > 120.0f) gpuT = 0.0f;
  m_cpuTemp = cpuT;
  m_gpuTemp = gpuT;
  m_cpuLoad = OmenHal::Get().GetCpuLoad();
  m_gpuLoad = OmenHal::Get().GetGpuLoad();

  float ramUsed = 0, ramTotal = 0, ramPct = 0;
  PowerControl::Get().GetSystemRamUsage(ramUsed, ramTotal, ramPct);
  m_ramLoad = ramPct;
  m_ramTemp = OmenHal::Get().GetRamTemp();

  if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HudWindow::ApplySavedPosition(float posX, float posY, float sizeW,
                                   float sizeH) {
  if (!m_hwnd) return;
  int w = (sizeW > 0) ? (int)sizeW : kWidth;
  int h = (sizeH > 0) ? (int)sizeH : kHeight;
  SetWindowPos(m_hwnd, HWND_TOPMOST, (int)posX, (int)posY, w, h,
               SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void HudWindow::SavePositionToConfig() {
  if (!m_hwnd) return;
  RECT rc;
  if (!GetWindowRect(m_hwnd, &rc)) return;
  auto &cfg = FanService::Get().GetOverlayConfig();
  cfg.posX = (float)rc.left;
  cfg.posY = (float)rc.top;
  cfg.sizeW = (float)(rc.right - rc.left);
  cfg.sizeH = (float)(rc.bottom - rc.top);
  FanService::Get().SaveConfig();
}

void HudWindow::SetPassthrough(bool on) {
  m_passthrough = on;
  if (!m_hwnd) return;
  LONG ex = GetWindowLongW(m_hwnd, GWL_EXSTYLE);
  if (on)
    ex |= WS_EX_TRANSPARENT;
  else
    ex &= ~(LONG)WS_EX_TRANSPARENT;
  SetWindowLongW(m_hwnd, GWL_EXSTYLE, ex);
  SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void HudWindow::SetOpacity(float opacity) {
  if (!m_hwnd) return;
  if (opacity < 0.0f) opacity = 0.0f;
  if (opacity > 1.0f) opacity = 1.0f;
  SetLayeredWindowAttributes(m_hwnd, 0, (BYTE)(opacity * 255.0f), LWA_ALPHA);
}

LRESULT CALLBACK HudWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  HudWindow *self = nullptr;
  if (msg == WM_NCCREATE) {
    auto *cs = (CREATESTRUCTW *)lp;
    self = (HudWindow *)cs->lpCreateParams;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  } else {
    self = (HudWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }

  switch (msg) {
  case WM_NCHITTEST:
    if (self) return self->OnNcHitTest(lp);
    break;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (self) self->OnPaint(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1; // handled in WM_PAINT (double-buffered)
  case WM_TIMER:
    if (self) self->RefreshFromHal();
    return 0;
  case WM_SIZE: {
    // Re-apply rounded region when resized.
    if (self) self->ApplyRoundRegion();
    return 0;
  }
  case WM_MOVING:
    // Real-time magnetic screen border snapping while dragging.
    if (self) {
      LPRECT rc = (LPRECT)lp;
      self->SnapToScreenBorders(rc);
    }
    return TRUE;
  case WM_EXITSIZEMOVE:
    // Drag/resize ended: snap to nearest screen border, then persist.
    if (self) {
      RECT rc;
      if (GetWindowRect(hwnd, &rc)) {
        self->SnapToScreenBorders(&rc);
        SetWindowPos(hwnd, nullptr, rc.left, rc.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      self->SavePositionToConfig();
    }
    return 0;
  case WM_GETMINMAXINFO: {
    auto *mmi = (MINMAXINFO *)lp;
    mmi->ptMinTrackSize.x = 90;
    mmi->ptMinTrackSize.y = 75;
    return 0;
  }
  case WM_CLOSE:
    if (self) self->Destroy();
    else DestroyWindow(hwnd);
    return 0;
  case WM_APP + 50:
    // Fullscreen app is in the foreground — reassert topmost (some games
    // demote us). Cheap, no-ops when already topmost.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return 0;
  case WM_DESTROY:
    if (self) {
      if (self->m_timerId) KillTimer(hwnd, self->m_timerId);
      self->m_timerId = 0;
      self->m_hwnd = nullptr;
    }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT HudWindow::OnNcHitTest(LPARAM lp) {
  if (m_passthrough) return HTTRANSPARENT;
  POINT pt = { LOWORD((DWORD)lp), HIWORD((DWORD)lp) };
  ScreenToClient(m_hwnd, &pt);
  RECT rc;
  GetClientRect(m_hwnd, &rc);
  if (pt.x >= rc.right - kResizeGrip && pt.y >= rc.bottom - kResizeGrip)
    return HTBOTTOMRIGHT;
  return HTCAPTION;
}

void HudWindow::SnapToScreenBorders(LPRECT rc) {
  // Determine which monitor the window is mostly on, snap to its work area.
  HMONITOR mon = MonitorFromRect(rc, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) return;
  const RECT &wa = mi.rcWork;

  const int snapDist = 24; // Expanded magnetic snap distance for smooth sticking
  int w = rc->right - rc->left;
  int h = rc->bottom - rc->top;

  if (std::abs(rc->left - wa.left) < snapDist) {
    rc->left = wa.left;
    rc->right = wa.left + w;
  } else if (std::abs(wa.right - rc->right) < snapDist) {
    rc->right = wa.right;
    rc->left = wa.right - w;
  }
  if (std::abs(rc->top - wa.top) < snapDist) {
    rc->top = wa.top;
    rc->bottom = wa.top + h;
  } else if (std::abs(wa.bottom - rc->bottom) < snapDist) {
    rc->bottom = wa.bottom;
    rc->top = wa.bottom - h;
  }
}

void HudWindow::OnPaint(HDC hdc) {
  RECT rc;
  GetClientRect(m_hwnd, &rc);
  int w = rc.right, h = rc.bottom;

  // Scale factors relative to the default 3-row layout (180x135)
  float sx = (float)w / kWidth;
  float sy = (float)h / kHeight;
  float s = std::min(sx, sy);

  // Double buffer to avoid flicker.
  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
  HGDIOBJ oldBmp = SelectObject(mem, bmp);

  // Background (match main window card: #0e0e12). No border — sleek, modern.
  HBRUSH bg = CreateSolidBrush(RGB(14, 14, 18));
  FillRect(mem, &rc, bg);
  DeleteObject(bg);

  // Fonts scale with window size.
  int fontPx = std::max(11, (int)(26 * s));
  HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
  HGDIOBJ oldFont = SelectObject(mem, font);

  SetBkMode(mem, TRANSPARENT);

  // Temperature coloring matching Main Window exactly:
  auto tempColor = [](float t) {
    if (t > 85) return RGB(230, 50, 50);    // Red
    if (t > 75) return RGB(230, 200, 50);   // Amber
    return RGB(50, 240, 180);               // Green
  };

  auto ramColor = [](float t) {
    if (t > 70) return RGB(230, 50, 50);    // Red
    if (t > 55) return RGB(230, 200, 50);   // Amber
    return RGB(50, 240, 180);               // Green
  };

  // Layout: label left, temp/pct right, thin glowing bars.
  int pad = std::max(4, (int)(8 * s));
  int textH = std::max(14, (int)(28 * s));
  int gapRow = std::max(3, (int)(6 * s));     // text row <-> bar
  int gapBetween = std::max(4, (int)(10 * s)); // between metric blocks
  int barH = std::max(2, (int)(4 * s));        // thin bar

  int blockH = 3 * (textH + gapRow + barH) + 2 * gapBetween;
  int topY = std::max(pad, (h - blockH) / 2);

  auto drawMetricRow = [&](int y, int barY, const wchar_t *label,
                           const wchar_t *valStr, COLORREF valColor, float load) {
    // Label left-aligned (dimmed — the values carry all attention).
    RECT lr = { pad, y, w / 2, y + textH };
    SetTextColor(mem, RGB(150, 150, 155));
    DrawTextW(mem, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Value right-aligned.
    RECT tr = { w / 2, y, w - pad, y + textH };
    SetTextColor(mem, valColor);
    DrawTextW(mem, valStr, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // Thin solid load bar; fill follows the same state color as the value.
    RECT track = { pad, barY, w - pad, barY + barH };
    HBRUSH tb = CreateSolidBrush(RGB(38, 38, 44));
    FillRect(mem, &track, tb);
    DeleteObject(tb);
    int fillW =
        (int)((float)(w - 2 * pad) * std::clamp(load / 100.0f, 0.0f, 1.0f));
    if (fillW > 0) {
      RECT fr = { pad, barY, pad + fillW, barY + barH };
      HBRUSH fb = CreateSolidBrush(valColor);
      FillRect(mem, &fr, fb);
      DeleteObject(fb);
    }
  };

  wchar_t tbuf[24];

  // 1. CPU
  int curY = topY;
  int barY0 = curY + textH + gapRow;
  swprintf_s(tbuf, L"%.0f\u00B0", m_cpuTemp);
  drawMetricRow(curY, barY0, L"CPU", tbuf, tempColor(m_cpuTemp), m_cpuLoad);

  // 2. GPU
  int y2 = barY0 + barH + gapBetween;
  int barY1 = y2 + textH + gapRow;
  if (m_gpuTemp > 0.0f) {
    swprintf_s(tbuf, L"%.0f\u00B0", m_gpuTemp);
    drawMetricRow(y2, barY1, L"GPU", tbuf, tempColor(m_gpuTemp), m_gpuLoad);
  } else {
    drawMetricRow(y2, barY1, L"GPU", L"Sleep", RGB(140, 140, 158), 0.0f);
  }

  // 3. RAM (temperature if available with ramColor matching main window; bar shows usage %)
  int y3 = barY1 + barH + gapBetween;
  int barY2 = y3 + textH + gapRow;
  if (m_ramTemp > 0.0f && m_ramTemp < 100.0f) {
    swprintf_s(tbuf, L"%.0f\u00B0", m_ramTemp);
    drawMetricRow(y3, barY2, L"RAM", tbuf, ramColor(m_ramTemp), m_ramLoad);
  } else {
    swprintf_s(tbuf, L"%.0f%%", m_ramLoad);
    COLORREF ramValColor = (m_ramLoad > 85.0f ? RGB(230, 50, 50)
                            : (m_ramLoad > 70.0f ? RGB(230, 200, 50) : RGB(50, 240, 180)));
    drawMetricRow(y3, barY2, L"RAM", tbuf, ramValColor, m_ramLoad);
  }

  SelectObject(mem, oldFont);
  DeleteObject(font);

  // Present.
  BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
  SelectObject(mem, oldBmp);
  DeleteObject(bmp);
  DeleteDC(mem);
}
