#include "CompactorWindow.h"
#include <algorithm>
#include <cstdio>
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
constexpr int kWinW = 620;
constexpr int kWinH = 550;
constexpr int kItemH = 72;
constexpr int kItemPad = 8;

static void fillRoundRect(Gdiplus::Graphics &gfx, float x, float y, float w, float h,
                          float r, const Gdiplus::Brush &brush,
                          const Gdiplus::Pen *pen = nullptr) {
  Gdiplus::GraphicsPath path;
  float d = r * 2.0f;
  path.AddArc(x, y, d, d, 180.0f, 90.0f);
  path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
  path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
  path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
  path.CloseFigure();
  gfx.FillPath(&brush, &path);
  if (pen) gfx.DrawPath(pen, &path);
}

static std::wstring FormatBytes(uint64_t bytes) {
  if (bytes == 0) return L"0 B";
  double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
  if (gb >= 1.0) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f GB", gb);
    return buf;
  }
  double mb = (double)bytes / (1024.0 * 1024.0);
  wchar_t buf[32];
  swprintf_s(buf, L"%.0f MB", mb);
  return buf;
}
} // namespace

CompactorWindow &CompactorWindow::Instance() {
  static CompactorWindow inst;
  return inst;
}

CompactorWindow::CompactorWindow() {
  RegisterClassOnce();
}

CompactorWindow::~CompactorWindow() {
  Destroy();
}

void CompactorWindow::RegisterClassOnce() {
  static bool registered = false;
  if (registered) return;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(14, 14, 18));
  wc.lpszClassName = L"AMDOMEN_COMPACTOR_WINDOW";
  RegisterClassExW(&wc);
  registered = true;
}

void CompactorWindow::ApplyRoundCorners() {
  if (!m_hwnd) return;
  RECT rc;
  GetClientRect(m_hwnd, &rc);
  HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, 10, 10);
  SetWindowRgn(m_hwnd, rgn, TRUE);
}

void CompactorWindow::Show(HWND hParent) {
  m_hParent = hParent;
  if (!m_hwnd) {
    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    if (hParent && IsWindow(hParent)) {
      RECT pr;
      GetWindowRect(hParent, &pr);
      posX = pr.left + (pr.right - pr.left - kWinW) / 2;
      posY = pr.top + (pr.bottom - pr.top - kWinH) / 2;
      if (posX < 10) posX = 10;
      if (posY < 10) posY = 10;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_APPWINDOW,
        L"AMDOMEN_COMPACTOR_WINDOW", L"Game Library Compactor",
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        posX, posY, kWinW, kWinH,
        hParent, nullptr, GetModuleHandleW(nullptr), this);

    if (!m_hwnd) return;

    // Set immersive dark title bar mode & Windows 11 rounded window corners
    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DWORD cornerPref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
    ApplyRoundCorners();
  }

  ShowWindow(m_hwnd, SW_SHOW);
  SetForegroundWindow(m_hwnd);

  if (!m_timerId) {
    m_timerId = SetTimer(m_hwnd, 101, 30, nullptr); // 30ms for smooth spinner animation
  }

  // Auto-scan if empty
  if (CompactorService::Get().GetGames().empty() && !CompactorService::Get().IsScanning()) {
    CompactorService::Get().StartScan(true);
  }
}

void CompactorWindow::Hide() {
  if (m_hwnd) {
    if (m_timerId) {
      KillTimer(m_hwnd, m_timerId);
      m_timerId = 0;
    }
    ShowWindow(m_hwnd, SW_HIDE);
  }
}

void CompactorWindow::Destroy() {
  if (m_hwnd) {
    if (m_timerId) {
      KillTimer(m_hwnd, m_timerId);
      m_timerId = 0;
    }
    DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
  }
  if (m_titleFont) { DeleteObject(m_titleFont); m_titleFont = nullptr; }
  if (m_boldFont) { DeleteObject(m_boldFont); m_boldFont = nullptr; }
  if (m_normFont) { DeleteObject(m_normFont); m_normFont = nullptr; }
  if (m_smallFont) { DeleteObject(m_smallFont); m_smallFont = nullptr; }
}

LRESULT CALLBACK CompactorWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  CompactorWindow *self = nullptr;
  if (msg == WM_NCCREATE) {
    CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
    self = reinterpret_cast<CompactorWindow *>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) self->m_hwnd = hwnd;
  } else {
    self = reinterpret_cast<CompactorWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  switch (msg) {
  case WM_NCHITTEST: {
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd, &pt);
    if (pt.y < 38 && pt.x < kWinW - 44) {
      return HTCAPTION;
    }
    return HTCLIENT;
  }
  case WM_WINDOWPOSCHANGED:
    if (self) self->ApplyRoundCorners();
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (self) self->OnPaint(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_LBUTTONDOWN:
    if (self) self->OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    return 0;
  case WM_MOUSEMOVE:
    if (self) self->OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    return 0;
  case WM_MOUSEWHEEL:
    if (self) self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
    return 0;
  case WM_TIMER:
    if (self) self->OnTimer();
    return 0;
  case WM_KEYDOWN:
    if (wp == VK_ESCAPE) {
      if (self) self->Hide();
      return 0;
    }
    break;
  case WM_CLOSE:
    if (self) self->Hide();
    return 0;
  case WM_DESTROY:
    if (self) {
      self->m_hwnd = nullptr;
      self->m_destroyed = true;
    }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void CompactorWindow::OnTimer() {
  m_spinnerAngle += 6.0f;
  if (m_spinnerAngle >= 360.0f) m_spinnerAngle -= 360.0f;

  if (m_hwnd) {
    InvalidateRect(m_hwnd, nullptr, FALSE);
  }
}

void CompactorWindow::OnMouseWheel(int delta) {
  m_scrollOffset -= (delta / WHEEL_DELTA) * 44;
  if (m_scrollOffset < 0) m_scrollOffset = 0;
  if (m_scrollOffset > m_maxScroll) m_scrollOffset = m_maxScroll;
  if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CompactorWindow::OnMouseMove(int x, int y) {
  bool isHand = false;
  POINT pt = { x, y };
  if (PtInRect(&m_rcClose, pt) || PtInRect(&m_rcCompactAll, pt) ||
      PtInRect(&m_rcRescan, pt) || PtInRect(&m_rcAlgoLzx, pt) || PtInRect(&m_rcAlgoXpress, pt)) {
    isHand = true;
  }
  for (const auto &ar : m_gameActionRects) {
    if (PtInRect(&ar.rcButton, pt)) {
      isHand = true;
      break;
    }
  }
  SetCursor(LoadCursorW(nullptr, isHand ? IDC_HAND : IDC_ARROW));
}

void CompactorWindow::OnLButtonDown(int x, int y) {
  POINT pt = { x, y };

  if (PtInRect(&m_rcClose, pt)) {
    Hide();
    return;
  }

  if (CompactorService::Get().IsScanning()) return;

  if (PtInRect(&m_rcRescan, pt)) {
    CompactorService::Get().StartScan(true);
    return;
  }

  if (PtInRect(&m_rcAlgoLzx, pt)) {
    CompactorService::Get().SetSelectedAlgo(CompactAlgo::LZX);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (PtInRect(&m_rcAlgoXpress, pt)) {
    CompactorService::Get().SetSelectedAlgo(CompactAlgo::XPRESS8K);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    return;
  }

  if (PtInRect(&m_rcCompactAll, pt)) {
    if (CompactorService::Get().IsBusy()) {
      CompactorService::Get().CancelOperation();
    } else {
      CompactorService::Get().StartCompactAll(CompactorService::Get().GetSelectedAlgo());
    }
    return;
  }

  for (const auto &ar : m_gameActionRects) {
    if (PtInRect(&ar.rcButton, pt)) {
      if (ar.isCancel) {
        CompactorService::Get().CancelOperation();
      } else if (ar.isDecompact) {
        CompactorService::Get().StartDecompact(ar.gameIndex);
      } else {
        CompactorService::Get().StartCompact(ar.gameIndex, CompactorService::Get().GetSelectedAlgo());
      }
      return;
    }
  }
}

void CompactorWindow::OnPaint(HDC hdc) {
  RECT cr;
  GetClientRect(m_hwnd, &cr);
  int w = cr.right - cr.left;
  int h = cr.bottom - cr.top;

  HDC memDC = CreateCompatibleDC(hdc);
  HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
  HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

  Gdiplus::Graphics gfx(memDC);
  gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

  // Obsidian Background
  Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 14, 14, 18));
  gfx.FillRectangle(&bgBrush, 0, 0, w, h);

  // Fonts initialization matching MainWindowWin32 exactly
  if (!m_titleFont) {
    m_titleFont = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_boldFont = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_normFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_smallFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  }

  // Outer Rounded Border
  Gdiplus::Pen borderPen(Gdiplus::Color(255, 42, 42, 54), 1.0f);
  fillRoundRect(gfx, 0.5f, 0.5f, (float)w - 1.0f, (float)h - 1.0f, 8.0f, Gdiplus::SolidBrush(Gdiplus::Color(0, 0, 0, 0)), &borderPen);

  // Top Title Bar
  {
    RECT tr = { 16, 8, w - 50, 34 };
    SetTextColor(memDC, RGB(240, 240, 245));
    SetBkMode(memDC, TRANSPARENT);
    SelectObject(memDC, m_titleFont);
    DrawTextW(memDC, L"Game Library Compactor (CompactOS)", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Close button [X]
    m_rcClose = { w - 38, 8, w - 10, 34 };
    RECT xRect = m_rcClose;
    SetTextColor(memDC, RGB(160, 160, 175));
    SelectObject(memDC, m_boldFont);
    DrawTextW(memDC, L"\u2715", -1, &xRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  // Divider
  Gdiplus::Pen divPen(Gdiplus::Color(255, 30, 30, 38), 1.0f);
  gfx.DrawLine(&divPen, 12.0f, 40.0f, (float)(w - 12), 40.0f);

  bool isScanning = CompactorService::Get().IsScanning();

  if (isScanning) {
    // ═══════════════════════════════════════════════════════════════════════
    // ANIMATED SCANNING SPINNER
    // ═══════════════════════════════════════════════════════════════════════
    float centerX = (float)w / 2.0f;
    float centerY = (float)h / 2.0f - 20.0f;
    float radius = 30.0f;

    // Track
    Gdiplus::Pen trackPen(Gdiplus::Color(255, 28, 28, 38), 4.0f);
    gfx.DrawEllipse(&trackPen, centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f);

    // Cyan rotating arc
    Gdiplus::Pen spinPen(Gdiplus::Color(255, 0, 212, 255), 4.0f);
    spinPen.SetStartCap(Gdiplus::LineCapRound);
    spinPen.SetEndCap(Gdiplus::LineCapRound);
    gfx.DrawArc(&spinPen, centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f,
                m_spinnerAngle, 110.0f);

    RECT lr = { 20, (int)centerY + 48, w - 20, (int)centerY + 72 };
    SetTextColor(memDC, RGB(240, 240, 245));
    SelectObject(memDC, m_boldFont);
    DrawTextW(memDC, L"Scanning Installed Game Libraries...", -1, &lr, DT_CENTER | DT_SINGLELINE);

    std::wstring status = CompactorService::Get().GetScanStatusText();
    RECT sr = { 20, (int)centerY + 74, w - 20, (int)centerY + 98 };
    SetTextColor(memDC, RGB(140, 140, 160));
    SelectObject(memDC, m_normFont);
    DrawTextW(memDC, status.c_str(), -1, &sr, DT_CENTER | DT_SINGLELINE);

  } else {
    // ═══════════════════════════════════════════════════════════════════════
    // DASHBOARD & GAMES LIST
    // ═══════════════════════════════════════════════════════════════════════
    auto games = CompactorService::Get().GetGames();
    uint64_t totalUncompressed = CompactorService::Get().GetTotalUncompressedBytes();
    uint64_t totalReclaimed = CompactorService::Get().GetTotalReclaimedBytes();

    // ── Header Summary Card ──
    Gdiplus::SolidBrush cardBg(Gdiplus::Color(255, 20, 20, 26));
    Gdiplus::Pen cardPen(Gdiplus::Color(255, 38, 38, 48), 1.0f);
    fillRoundRect(gfx, 12.0f, 48.0f, (float)(w - 24), 72.0f, 6.0f, cardBg, &cardPen);

    // Summary stats
    wchar_t statStr[128];
    double savedPct = (totalUncompressed > 0) ? ((double)totalReclaimed / (double)totalUncompressed) * 100.0 : 0.0;
    swprintf_s(statStr, L"%zu Games Found   \u2022   Total: %ls   \u2022   Saved: %ls (%.1f%%)",
               games.size(), FormatBytes(totalUncompressed).c_str(),
               FormatBytes(totalReclaimed).c_str(), savedPct);

    RECT strRect = { 24, 52, w - 24, 76 };
    SetTextColor(memDC, RGB(235, 235, 240));
    SelectObject(memDC, m_boldFont);
    DrawTextW(memDC, statStr, -1, &strRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Algorithm selector pills (height 28, radius 6, matching MainWindow buttons)
    CompactAlgo currentAlgo = CompactorService::Get().GetSelectedAlgo();

    m_rcAlgoLzx = { 24, 82, 134, 110 };
    bool isLzx = (currentAlgo == CompactAlgo::LZX);
    Gdiplus::SolidBrush lzxBg(isLzx ? Gdiplus::Color(255, 28, 48, 40) : Gdiplus::Color(255, 28, 28, 38));
    Gdiplus::Pen lzxPen(isLzx ? Gdiplus::Color(255, 50, 240, 180) : Gdiplus::Color(255, 52, 52, 66), 1.0f);
    fillRoundRect(gfx, (float)m_rcAlgoLzx.left, (float)m_rcAlgoLzx.top,
                  (float)(m_rcAlgoLzx.right - m_rcAlgoLzx.left), (float)(m_rcAlgoLzx.bottom - m_rcAlgoLzx.top),
                  6.0f, lzxBg, &lzxPen);
    SetTextColor(memDC, isLzx ? RGB(50, 240, 180) : RGB(220, 220, 230));
    SelectObject(memDC, m_normFont);
    DrawTextW(memDC, L"LZX (Max)", -1, &m_rcAlgoLzx, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    m_rcAlgoXpress = { 142, 82, 272, 110 };
    bool isXp = (currentAlgo == CompactAlgo::XPRESS8K);
    Gdiplus::SolidBrush xpBg(isXp ? Gdiplus::Color(255, 28, 48, 40) : Gdiplus::Color(255, 28, 28, 38));
    Gdiplus::Pen xpPen(isXp ? Gdiplus::Color(255, 50, 240, 180) : Gdiplus::Color(255, 52, 52, 66), 1.0f);
    fillRoundRect(gfx, (float)m_rcAlgoXpress.left, (float)m_rcAlgoXpress.top,
                  (float)(m_rcAlgoXpress.right - m_rcAlgoXpress.left), (float)(m_rcAlgoXpress.bottom - m_rcAlgoXpress.top),
                  6.0f, xpBg, &xpPen);
    SetTextColor(memDC, isXp ? RGB(50, 240, 180) : RGB(220, 220, 230));
    DrawTextW(memDC, L"XPRESS8K (Fast)", -1, &m_rcAlgoXpress, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Rescan button (matching MainWindow button style)
    m_rcRescan = { w - 236, 82, w - 146, 110 };
    Gdiplus::SolidBrush rescanBg(Gdiplus::Color(255, 28, 28, 38));
    Gdiplus::Pen rescanPen(Gdiplus::Color(255, 52, 52, 66), 1.0f);
    fillRoundRect(gfx, (float)m_rcRescan.left, (float)m_rcRescan.top,
                  (float)(m_rcRescan.right - m_rcRescan.left), (float)(m_rcRescan.bottom - m_rcRescan.top),
                  6.0f, rescanBg, &rescanPen);
    SetTextColor(memDC, RGB(235, 235, 240));
    SelectObject(memDC, m_normFont);
    DrawTextW(memDC, L"Rescan", -1, &m_rcRescan, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Compact All button (matching MainWindow button style)
    bool isBusy = CompactorService::Get().IsBusy();
    m_rcCompactAll = { w - 138, 82, w - 24, 110 };
    Gdiplus::SolidBrush caBg(isBusy ? Gdiplus::Color(255, 60, 24, 28) : Gdiplus::Color(255, 20, 52, 36));
    Gdiplus::Pen caPen(isBusy ? Gdiplus::Color(255, 230, 80, 90) : Gdiplus::Color(255, 50, 240, 180), 1.0f);
    fillRoundRect(gfx, (float)m_rcCompactAll.left, (float)m_rcCompactAll.top,
                  (float)(m_rcCompactAll.right - m_rcCompactAll.left), (float)(m_rcCompactAll.bottom - m_rcCompactAll.top),
                  6.0f, caBg, &caPen);
    SetTextColor(memDC, isBusy ? RGB(255, 100, 110) : RGB(50, 240, 180));
    SelectObject(memDC, m_boldFont);
    DrawTextW(memDC, isBusy ? L"Cancel" : L"Compact All", -1, &m_rcCompactAll, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // ── Scrollable Games List ──
    int listY = 128;
    int listH = h - listY - 12;
    int totalContentH = (int)games.size() * (kItemH + kItemPad);
    m_maxScroll = std::max(0, totalContentH - listH);

    HRGN clipRgn = CreateRectRgn(12, listY, w - 12, h - 12);
    SelectClipRgn(memDC, clipRgn);

    m_gameActionRects.clear();

    if (games.empty()) {
      RECT noGamesRect = { 20, listY + 60, w - 20, listY + 120 };
      SetTextColor(memDC, RGB(140, 140, 160));
      SelectObject(memDC, m_normFont);
      DrawTextW(memDC, L"No games found. Ensure Steam, Epic, or Hydra games are installed on this system.",
                -1, &noGamesRect, DT_CENTER | DT_WORDBREAK);
    } else {
      int curY = listY - m_scrollOffset;

      for (size_t i = 0; i < games.size(); i++) {
        const auto &g = games[i];

        if (curY + kItemH >= listY && curY <= listY + listH) {
          float itemX = 12.0f;
          float itemW = (float)(w - 24);

          // Card background (matching MainWindow card styling, rounded 6.0f)
          Gdiplus::SolidBrush gCardBg(Gdiplus::Color(255, 20, 20, 26));
          Gdiplus::Pen gCardPen(Gdiplus::Color(255, 34, 34, 46), 1.0f);
          fillRoundRect(gfx, itemX, (float)curY, itemW, (float)kItemH, 6.0f, gCardBg, &gCardPen);

          // Launcher Badge
          Gdiplus::Color badgeBg = Gdiplus::Color(255, 27, 40, 56);
          Gdiplus::Color badgeBorder = Gdiplus::Color(255, 42, 71, 94);
          COLORREF badgeTxt = RGB(0, 180, 240);

          if (g.launcher == L"Epic") {
            badgeBg = Gdiplus::Color(255, 20, 42, 40);
            badgeBorder = Gdiplus::Color(255, 0, 150, 136);
            badgeTxt = RGB(0, 220, 180);
          } else if (g.launcher == L"Hydra") {
            badgeBg = Gdiplus::Color(255, 40, 20, 55);
            badgeBorder = Gdiplus::Color(255, 155, 89, 182);
            badgeTxt = RGB(190, 130, 240);
          }

          Gdiplus::SolidBrush bBrush(badgeBg);
          Gdiplus::Pen bPen(badgeBorder, 1.0f);
          fillRoundRect(gfx, itemX + 12.0f, (float)curY + 12.0f, 48.0f, 20.0f, 4.0f, bBrush, &bPen);

          RECT badgeRect = { (int)itemX + 12, curY + 12, (int)itemX + 60, curY + 32 };
          SetTextColor(memDC, badgeTxt);
          SelectObject(memDC, m_smallFont);
          DrawTextW(memDC, g.launcher.c_str(), -1, &badgeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

          // DirectStorage Badge (Amber pill if game utilizes Microsoft DirectStorage)
          int titleLeft = (int)itemX + 68;
          if (g.hasDirectStorage) {
            float dsX = itemX + 65.0f;
            float dsW = 90.0f;
            Gdiplus::SolidBrush dsBrush(Gdiplus::Color(255, 45, 32, 16));
            Gdiplus::Pen dsPen(Gdiplus::Color(255, 230, 160, 30), 1.0f);
            fillRoundRect(gfx, dsX, (float)curY + 12.0f, dsW, 20.0f, 4.0f, dsBrush, &dsPen);

            RECT dsRect = { (int)dsX, curY + 12, (int)(dsX + dsW), curY + 32 };
            SetTextColor(memDC, RGB(255, 175, 45));
            SelectObject(memDC, m_smallFont);
            DrawTextW(memDC, L"DirectStorage", -1, &dsRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            titleLeft += 94;
          }

          // Game Title (Segoe UI bold 14px)
          RECT titleRect = { titleLeft, curY + 11, (int)itemX + w - 160, curY + 33 };
          SetTextColor(memDC, RGB(240, 240, 245));
          SelectObject(memDC, m_boldFont);
          DrawTextW(memDC, g.title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

          // Row 2: Storage breakdown + Worthiness analysis note
          wchar_t sizeBuf[160] = {};
          if (g.state == GameCompactionState::Compacting || g.state == GameCompactionState::Decompacting) {
            if (!g.currentFile.empty()) {
              swprintf_s(sizeBuf, L"%ls (%.0f%%) \u2022 %ls%ls",
                         (g.state == GameCompactionState::Compacting ? L"Compacting" : L"Decompacting"),
                         g.progress,
                         g.currentFile.c_str(),
                         g.retries > 1 ? L" [Retry]" : L"");
            } else {
              swprintf_s(sizeBuf, L"%ls... (%.0f%%) %ls",
                         (g.state == GameCompactionState::Compacting ? L"Compacting" : L"Decompacting"),
                         g.progress,
                         g.retries > 1 ? L"[Retry]" : L"");
            }
          } else if (g.state == GameCompactionState::InQueue) {
            swprintf_s(sizeBuf, L"Queued in batch...");
          } else if (g.state == GameCompactionState::Compacted && g.compressedBytes > 0) {
            swprintf_s(sizeBuf, L"%ls on disk   \u2022   Saved: %ls (%.0f%%)%ls",
                       FormatBytes(g.compressedBytes).c_str(),
                       FormatBytes(g.uncompressedBytes - g.compressedBytes).c_str(),
                       g.savingsPercent,
                       g.hasDirectStorage ? L"   \u2022   \u26A0 DirectStorage" : L"");
          } else {
            if (!g.analysisNote.empty()) {
              swprintf_s(sizeBuf, L"%ls   \u2022   %ls", FormatBytes(g.uncompressedBytes).c_str(), g.analysisNote.c_str());
            } else {
              swprintf_s(sizeBuf, L"%ls on disk   \u2022   Not Compacted", FormatBytes(g.uncompressedBytes).c_str());
            }
          }

          RECT subRect = { (int)itemX + 14, curY + 38, (int)itemX + w - 160, curY + 62 };
          if (g.hasDirectStorage && g.state != GameCompactionState::Compacted) {
            SetTextColor(memDC, RGB(255, 175, 45)); // Amber warning
          } else if (g.state == GameCompactionState::Compacted) {
            SetTextColor(memDC, RGB(50, 240, 180)); // Electric green
          } else if (g.state == GameCompactionState::Compacting || g.state == GameCompactionState::Decompacting) {
            SetTextColor(memDC, RGB(0, 212, 255)); // Cyan
          } else if (g.rating == CompressibilityRating::High) {
            SetTextColor(memDC, RGB(180, 220, 200));
          } else {
            SetTextColor(memDC, RGB(140, 140, 155));
          }
          SelectObject(memDC, m_normFont);
          DrawTextW(memDC, sizeBuf, -1, &subRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

          // Action Button (matching main window button size & font)
          int btnW = 100;
          int btnH = 30;
          int btnX = (int)itemX + (int)itemW - btnW - 14;
          int btnY = curY + (kItemH - btnH) / 2;

          RECT btnRect = { btnX, btnY, btnX + btnW, btnY + btnH };
          GameActionRect gar;
          gar.gameIndex = i;
          gar.rcButton = btnRect;
          gar.isDecompact = (g.state == GameCompactionState::Compacted);
          gar.isCancel = (g.state == GameCompactionState::Compacting || g.state == GameCompactionState::Decompacting);
          m_gameActionRects.push_back(gar);

          Gdiplus::Color actBg = Gdiplus::Color(255, 28, 28, 38);
          Gdiplus::Color actBorder = Gdiplus::Color(255, 52, 52, 66);
          COLORREF actTxt = RGB(235, 235, 240);
          const wchar_t *btnLabel = L"Compact";

          if (gar.isCancel) {
            actBg = Gdiplus::Color(255, 60, 24, 28);
            actBorder = Gdiplus::Color(255, 230, 80, 90);
            actTxt = RGB(255, 100, 110);
            btnLabel = L"Cancel";
          } else if (gar.isDecompact) {
            actBg = Gdiplus::Color(255, 28, 28, 38);
            actBorder = Gdiplus::Color(255, 52, 52, 66);
            actTxt = RGB(200, 200, 215);
            btnLabel = L"Decompact";
          } else if (g.hasDirectStorage) {
            // Amber caution styling for DirectStorage overrides
            actBg = Gdiplus::Color(255, 45, 32, 16);
            actBorder = Gdiplus::Color(255, 230, 160, 30);
            actTxt = RGB(255, 175, 45);
          } else {
            actBg = Gdiplus::Color(255, 20, 52, 36);
            actBorder = Gdiplus::Color(255, 50, 240, 180);
            actTxt = RGB(50, 240, 180);
          }

          Gdiplus::SolidBrush actBrush(actBg);
          Gdiplus::Pen actPen(actBorder, 1.0f);
          fillRoundRect(gfx, (float)btnX, (float)btnY, (float)btnW, (float)btnH, 6.0f, actBrush, &actPen);

          SetTextColor(memDC, actTxt);
          SelectObject(memDC, m_normFont); // Exactly same font as main window buttons!
          DrawTextW(memDC, btnLabel, -1, &btnRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        curY += (kItemH + kItemPad);
      }
    }

    SelectClipRgn(memDC, NULL);
    DeleteObject(clipRgn);

    // Custom Scrollbar
    if (m_maxScroll > 0) {
      int barX = w - 8;
      int barY = listY;
      int barH = listH;

      Gdiplus::SolidBrush trackBg(Gdiplus::Color(255, 20, 20, 26));
      gfx.FillRectangle(&trackBg, barX, barY, 4, barH);

      float thumbRatio = (float)listH / (float)totalContentH;
      int thumbH = std::max(24, (int)(barH * thumbRatio));
      int thumbY = barY + (int)((float)m_scrollOffset / (float)m_maxScroll * (barH - thumbH));

      Gdiplus::SolidBrush thumbBg(Gdiplus::Color(255, 60, 60, 80));
      fillRoundRect(gfx, (float)barX, (float)thumbY, 4.0f, (float)thumbH, 2.0f, thumbBg);
    }
  }

  BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

  SelectObject(memDC, oldBmp);
  DeleteObject(memBmp);
  DeleteDC(memDC);
}
