#include "Overlay.h"
#include "hal/FanService.h"
#include "hal/OmenHal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

// DWM Types for dynamic loading
typedef struct _MARGINS {
  int cxLeftWidth;
  int cxRightWidth;
  int cyTopHeight;
  int cyBottomHeight;
} MARGINS, *PMARGINS;
typedef HRESULT(WINAPI *DwmExtendFrameIntoClientAreaFunc)(HWND,
                                                          const MARGINS *);

#define WM_TRAYICON (WM_USER + 1)

// ---- Helper Functions ----

static ImVec4 TempColor(float t, float warn, float crit) {
  if (t >= crit)
    return ImVec4(1.0f, 0.2f, 0.1f, 1.0f);
  if (t >= warn)
    return ImVec4(1.0f, 0.62f, 0.0f, 1.0f);
  return ImVec4(0.3f, 1.0f, 0.5f, 1.0f);
}

// Lerp between two ImU32 colors
static ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
  float ar = ((a >> 0) & 0xFF) / 255.0f;
  float ag = ((a >> 8) & 0xFF) / 255.0f;
  float ab = ((a >> 16) & 0xFF) / 255.0f;
  float aa = ((a >> 24) & 0xFF) / 255.0f;
  float br = ((b >> 0) & 0xFF) / 255.0f;
  float bg = ((b >> 8) & 0xFF) / 255.0f;
  float bb = ((b >> 16) & 0xFF) / 255.0f;
  float ba = ((b >> 24) & 0xFF) / 255.0f;
  return IM_COL32(
      (int)((ar + (br - ar) * t) * 255), (int)((ag + (bg - ag) * t) * 255),
      (int)((ab + (bb - ab) * t) * 255), (int)((aa + (ba - aa) * t) * 255));
}

// NOTHING PHONE style dot-gauge with centered text and dynamic coloring
// warn/crit are fractions 0..1 (not the raw temp values)
static void DrawRingGauge(ImDrawList *dl, ImVec2 center, float radius,
                          float val, float maxVal, float warnFrac,
                          float critFrac, const char *label,
                          ImFont *font = nullptr) {
  const float kPi = 3.14159265f;
  // Arc span: 300 degrees as requested (allows space at bottom)
  const float arcSpan = kPi * 1.666f;
  const float arcStart = kPi * 0.5f + (kPi * 2.0f - arcSpan) * 0.5f;

  float pct = std::clamp(val / maxVal, 0.0f, 1.0f);
  float thickness = std::max(3.0f, radius * 0.22f);
  int numDots = 20; // Reduced dots for smaller size

  // Background dots (track)
  for (int i = 0; i < numDots; i++) {
    float angle = arcStart + (arcSpan / numDots) * i;
    float dx = cosf(angle), dy = sinf(angle);
    ImVec2 p(center.x + dx * radius, center.y + dy * radius);
    dl->AddCircleFilled(p, thickness * 0.35f, IM_COL32(255, 255, 255, 25));
  }

  // Active dots
  ImU32 valColor =
      ImGui::GetColorU32(TempColor(val, warnFrac * maxVal, critFrac * maxVal));
  int activeDots = (int)(pct * numDots);
  if (pct > 0.0f && activeDots == 0)
    activeDots = 1;

  for (int i = 0; i < activeDots; i++) {
    float angle = arcStart + (arcSpan / numDots) * i;
    float dx = cosf(angle), dy = sinf(angle);
    ImVec2 p(center.x + dx * radius, center.y + dy * radius);
    dl->AddCircleFilled(p, thickness * 0.45f, valColor);

    // Tip glow for the last dot
    if (i == activeDots - 1) {
      dl->AddCircleFilled(p, thickness * 0.70f, IM_COL32(255, 255, 255, 80));
    }
  }

  // Centered temperature text (10% bigger)
  float fontSizeScale = radius * 0.75f;
  char valBuf[16];
  sprintf(valBuf, "%.0f C", val);  // Simple C since degree overlaps font tables

  ImVec2 tSz = font ? font->CalcTextSizeA(fontSizeScale, FLT_MAX, 0.0f, valBuf)
                    : ImGui::CalcTextSize(valBuf);
  dl->AddText(font, fontSizeScale,
              ImVec2(center.x - tSz.x * 0.5f, center.y - tSz.y * 0.6f),
              valColor, valBuf);

  // Label text — warm near-white
  float labelScale = radius * 0.50f;
  ImVec2 lSz = font ? font->CalcTextSizeA(labelScale, FLT_MAX, 0.0f, label)
                    : ImGui::CalcTextSize(label);
  dl->AddText(font, labelScale,
              ImVec2(center.x - lSz.x * 0.5f, center.y + radius + 2.0f),
              IM_COL32(210, 190, 190, 220), label);
}

static void DrawFanModern(ImDrawList *dl, ImVec2 center, float radius,
                          float angle, ImU32 color) {
  dl->AddCircle(center, radius + 1, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f)),
                32, 1.0f);
  for (int i = 0; i < 4; i++) {
    float a = angle + (i * 1.5708f);
    ImVec2 p1(center.x + cosf(a) * radius, center.y + sinf(a) * radius);
    ImVec2 p2(center.x + cosf(a + 0.38f) * (radius * 0.5f),
              center.y + sinf(a + 0.38f) * (radius * 0.5f));
    dl->AddTriangleFilled(center, p1, p2, color);
  }
}

static void DrawCpuIcon(ImDrawList *dl, ImVec2 p, float s, ImU32 color) {
  dl->AddRect(p, ImVec2(p.x + s, p.y + s), color, 0.0f, 0, 1.2f);
  for (int i = 0; i < 3; i++) {
    dl->AddLine(ImVec2(p.x + 2 + i * 3, p.y - 1), ImVec2(p.x + 2 + i * 3, p.y),
                color);
    dl->AddLine(ImVec2(p.x + 2 + i * 3, p.y + s),
                ImVec2(p.x + 2 + i * 3, p.y + s + 1), color);
  }
}
static void DrawGpuIcon(ImDrawList *dl, ImVec2 p, float s, ImU32 color) {
  dl->AddRect(ImVec2(p.x, p.y + 1), ImVec2(p.x + s + 2, p.y + s - 1), color,
              1.0f, 0, 1.2f);
  dl->AddCircle(ImVec2(p.x + (s + 2) * 0.5f, p.y + s * 0.5f + 1), s * 0.25f,
                color, 12, 1.0f);
}
static void DrawDiskIcon(ImDrawList *dl, ImVec2 p, float s, ImU32 color) {
  dl->AddRect(ImVec2(p.x, p.y + 2), ImVec2(p.x + s + 2, p.y + s - 2), color,
              1.0f, 0, 1.2f);
  dl->AddRectFilled(ImVec2(p.x + 2, p.y + 4), ImVec2(p.x + 4, p.y + 6), color);
}

// ---- Overlay Class Implementation ----

Overlay::Overlay()
    : m_draggingIdx(-1), m_draggingId(nullptr), m_showEditor(false),
      m_showOptions(false), m_showOverlayHUD(false), m_hudOpacity(0.9f),
      m_hudSize(ImVec2(220, 130)), m_hudResizable(false),
      m_hudAlwaysOnTop(false), m_hudVertical(false), m_hudPos(ImVec2(100, 100)),
      m_hwnd(NULL), m_trayMode(false), m_trayIcon(NULL) {
  ZeroMemory(&m_nid, sizeof(m_nid));

  // Load from FanService config
  auto &c = FanService::Get().GetOverlayConfig();
  m_showOverlayHUD = c.show;
  m_hudAlwaysOnTop = c.top;
  m_hudVertical = c.vertical;
  m_hudOpacity = c.opacity;
  m_hudPos = ImVec2(c.posX, c.posY);
}

Overlay::~Overlay() { RemoveTrayIcon(); }

bool Overlay::IsAutoStartEnabled() const {
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_READ, &hKey) == ERROR_SUCCESS) {
    WCHAR path[MAX_PATH];
    DWORD pathLen = sizeof(path);
    if (RegQueryValueExW(hKey, L"OmenControlTool", NULL, NULL, (LPBYTE)path,
                         &pathLen) == ERROR_SUCCESS) {
      RegCloseKey(hKey);
      return true;
    }
    RegCloseKey(hKey);
  }
  return false;
}

void Overlay::SetAutoStart(bool enable) {
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
    if (enable) {
      WCHAR path[MAX_PATH];
      GetModuleFileNameW(NULL, path, MAX_PATH);
      RegSetValueExW(hKey, L"OmenControlTool", 0, REG_SZ, (LPBYTE)path,
                     (wcslen(path) + 1) * sizeof(WCHAR));
    } else {
      RegDeleteValueW(hKey, L"OmenControlTool");
    }
    RegCloseKey(hKey);
  }
}

void Overlay::SyncConfig() {
  FanService::OverlayConfig c;
  c.show = m_showOverlayHUD;
  c.top = m_hudAlwaysOnTop;
  c.vertical = m_hudVertical;
  c.opacity = m_hudOpacity;
  c.posX = m_hudPos.x;
  c.posY = m_hudPos.y;
  FanService::Get().SetOverlayConfig(c);
}

// ---- Tray Logic ----
void Overlay::SetupTrayIcon() {
  if (!m_hwnd)
    return;
  m_nid.cbSize = sizeof(NOTIFYICONDATAW);
  m_nid.hWnd = m_hwnd;
  m_nid.uID = 1;
  m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  m_nid.uCallbackMessage = WM_TRAYICON;

  m_nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
  if (!m_nid.hIcon)
    m_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);

  wcscpy(m_nid.szTip, L"Omen Control Tool");
  Shell_NotifyIconW(NIM_ADD, &m_nid);
}

void Overlay::RemoveTrayIcon() {
  if (m_nid.hWnd)
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
  if (m_trayIcon) {
    DestroyIcon(m_trayIcon);
    m_trayIcon = NULL;
  }
}

void Overlay::UpdateTrayIcon(float /*cpuTemp*/, float /*gpuTemp*/) {}

void Overlay::RestoreFromTray() {
  if (!m_hwnd)
    return;
  m_trayMode = false;
  RemoveTrayIcon();
  ShowWindow(m_hwnd, SW_RESTORE);
  SetForegroundWindow(m_hwnd);
}

void Overlay::HandleTrayMenu() {
  if (!m_hwnd)
    return;
  HMENU hMenu = CreatePopupMenu();

  if (m_showOverlayHUD)
    AppendMenuW(hMenu, MF_STRING, 2001, L"Disable Overlay");
  else
    AppendMenuW(hMenu, MF_STRING, 2001, L"Enable Overlay");

  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  if (IsAutoStartEnabled())
    AppendMenuW(hMenu, MF_STRING, 2003, L"Disable AutoStart");
  else
    AppendMenuW(hMenu, MF_STRING, 2003, L"Enable AutoStart");
  AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(hMenu, MF_STRING, 2002, L"Exit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(m_hwnd);

  int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                           m_hwnd, NULL);
  DestroyMenu(hMenu);

  if (cmd == 2001) {
    m_showOverlayHUD = !m_showOverlayHUD;
    SyncConfig();
  } else if (cmd == 2002) {
    PostQuitMessage(0);
  } else if (cmd == 2003) {
    SetAutoStart(!IsAutoStartEnabled());
  }
}

// ---- Main Render ----

void Overlay::Render(OmenHal &hal) {
  // ── Design Tokens ─────────────────────────────────────────────────────────
  ImGuiStyle &style = ImGui::GetStyle();
  style.FrameRounding   = 3.0f;
  style.WindowRounding  = 0.0f;
  style.GrabRounding    = 2.0f;
  style.ScrollbarRounding = 2.0f;
  style.FramePadding    = ImVec2(6, 3);   // compact
  style.ItemSpacing     = ImVec2(6, 4);   // compact vertical rhythm
  style.WindowPadding   = ImVec2(10, 8);  // tight window padding

  // Pure red palette — OMEN brand, no orange
  const ImVec4 kBg       = ImVec4(0.06f, 0.05f, 0.05f, 1.0f); // near-black, warm
  const ImVec4 kSurface  = ImVec4(0.10f, 0.08f, 0.08f, 1.0f); // dark warm surface
  const ImVec4 kElevated = ImVec4(0.15f, 0.12f, 0.12f, 1.0f); // elevated surface
  const ImVec4 kAccent   = ImVec4(0.82f, 0.10f, 0.10f, 1.0f); // OMEN red
  const ImVec4 kAccentDim= ImVec4(0.50f, 0.06f, 0.06f, 0.90f); // dimmed red
  const ImVec4 kText     = ImVec4(0.92f, 0.90f, 0.90f, 1.0f); // warm off-white
  const ImVec4 kMuted    = ImVec4(0.45f, 0.42f, 0.42f, 1.0f); // warm muted
  const ImVec4 kGreen    = ImVec4(0.24f, 0.80f, 0.40f, 1.0f);
  const ImVec4 kOrange   = ImVec4(0.95f, 0.65f, 0.08f, 1.0f);
  const ImVec4 kRed      = ImVec4(0.82f, 0.10f, 0.10f, 1.0f); // same as accent
  const ImVec4 kBorder   = ImVec4(0.20f, 0.15f, 0.15f, 1.0f); // warm border

  const ImU32 kAccentU32 = ImGui::ColorConvertFloat4ToU32(kAccent);
  const ImU32 kBorderU32 = ImGui::ColorConvertFloat4ToU32(kBorder);

  style.Colors[ImGuiCol_WindowBg]        = kBg;
  style.Colors[ImGuiCol_ChildBg]         = kSurface;
  style.Colors[ImGuiCol_Button]          = kElevated;
  style.Colors[ImGuiCol_ButtonHovered]   = ImVec4(0.50f, 0.08f, 0.08f, 0.80f);
  style.Colors[ImGuiCol_ButtonActive]    = kAccentDim;
  style.Colors[ImGuiCol_SliderGrab]      = kAccent;
  style.Colors[ImGuiCol_SliderGrabActive]= ImVec4(1.0f, 0.30f, 0.30f, 1.0f);
  style.Colors[ImGuiCol_CheckMark]       = kAccent;
  style.Colors[ImGuiCol_FrameBg]         = kSurface;
  style.Colors[ImGuiCol_FrameBgHovered]  = kElevated;
  style.Colors[ImGuiCol_FrameBgActive]   = kElevated;
  style.Colors[ImGuiCol_Separator]       = kBorder;
  style.Colors[ImGuiCol_Text]            = kText;
  style.Colors[ImGuiCol_TextDisabled]    = kMuted;
  style.Colors[ImGuiCol_PopupBg]         = kSurface;
  style.Colors[ImGuiCol_ScrollbarBg]     = kBg;
  style.Colors[ImGuiCol_ScrollbarGrab]   = kElevated;
  style.Colors[ImGuiCol_Header]          = kElevated;
  style.Colors[ImGuiCol_HeaderHovered]   = kAccentDim;
  style.Colors[ImGuiCol_Tab]             = kBg;
  style.Colors[ImGuiCol_TabHovered]      = kElevated;
  style.Colors[ImGuiCol_TabSelected]     = kAccentDim;
  style.Colors[ImGuiCol_TabSelectedOverline] = kAccent;

  const float btnH = 22.0f; // compact buttons

  // ── 1. MAIN WINDOW ─────────────────────────────────────────────────────
  if (!m_trayMode) {
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    // Fill full viewport; no auto-resize on the outer window since it fills
    // the OS window which itself is resizable (WS_SIZEBOX)
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin("OmenControlTool", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    // Dynamic height: resize OS window to fit content (height only, width stays fixed)
    static float s_lastContentH = 0;
    static int   s_fixedClientW = 0; // capture on first frame
    if (s_lastContentH > 0) {
      HWND hwnd = (HWND)viewport->PlatformHandle;
      if (hwnd) {
        // Capture client width once and lock it
        if (s_fixedClientW == 0) {
          RECT cr0; GetClientRect(hwnd, &cr0);
          s_fixedClientW = cr0.right - cr0.left;
        }
        RECT cr; GetClientRect(hwnd, &cr);
        int curH = cr.bottom - cr.top;
        int wantH = (int)std::ceil(s_lastContentH);
        int curW  = cr.right - cr.left;
        bool needH = std::abs(curH - wantH) > 2;
        bool needW = (s_fixedClientW > 0) && std::abs(curW - s_fixedClientW) > 2;
        if (needH || needW) {
          RECT adj = {0, 0, s_fixedClientW > 0 ? s_fixedClientW : curW, wantH};
          AdjustWindowRect(&adj, GetWindowLong(hwnd, GWL_STYLE), FALSE);
          SetWindowPos(hwnd, NULL, 0, 0,
                       adj.right - adj.left, adj.bottom - adj.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
    }

    float width = ImGui::GetContentRegionAvail().x;
    ImDrawList *wdl = ImGui::GetWindowDrawList();

    // ── Header ─────────────────────────────────────────────────────
    {
      ImVec2 hp = ImGui::GetCursorScreenPos();
      wdl->AddRectFilled(
          ImVec2(hp.x - 10, hp.y - 8), ImVec2(hp.x + width + 10, hp.y + 22),
          IM_COL32(13, 10, 10, 255));
      wdl->AddRectFilled(ImVec2(hp.x - 10, hp.y + 20),
                         ImVec2(hp.x + width + 10, hp.y + 22), kAccentU32);
      ImGui::TextColored(kAccent, "OMEN");
      ImGui::SameLine(0, 5);
      ImGui::TextColored(kText, "Replace");

      int ecErrs = hal.GetEcErrorCount();
      if (ecErrs > 0) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(width - 80);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::Text("ERR:%d", ecErrs);
        ImGui::PopStyleColor();
      }
      ImGui::Dummy(ImVec2(0, 8));
    }

    if (hal.GetIsDesktop() && hal.GetIsAnotherFanControllerActive()) {
      ImVec2 wp = ImGui::GetCursorScreenPos();
      wdl->AddRectFilled(
          wp, ImVec2(wp.x + width, wp.y + 28),
          IM_COL32(60, 20, 10, 255), 3.0f);
      wdl->AddRectFilled(wp, ImVec2(wp.x + 3, wp.y + 28),
                         kAccentU32, 3.0f);
      ImGui::SetCursorScreenPos(ImVec2(wp.x + 8, wp.y + 6));
      ImGui::TextColored(kOrange, "Hub active — fan conflicts possible");
      ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + 32));
    }

    // Tab colors already applied via style.Colors above
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));

    if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {

      // ── DASHBOARD TAB ────────────────────────────────────────────────
      if (ImGui::BeginTabItem("Dashboard")) {
        ImGui::Dummy(ImVec2(0, 4));

        // Compact section label: 2px red bar + muted uppercase text
        auto SectionStart = [&](const char *title, const char *rightText = nullptr) {
          ImVec2 sp = ImGui::GetCursorScreenPos();
          wdl->AddRectFilled(ImVec2(sp.x, sp.y + 2),
                             ImVec2(sp.x + 2, sp.y + 12), kAccentU32);
          ImGui::SetCursorScreenPos(ImVec2(sp.x + 6, sp.y));
          ImGui::TextColored(kMuted, "%s", title);
          if (rightText && rightText[0] != '\0') {
              ImVec2 rs = ImGui::CalcTextSize(rightText);
              ImGui::SetCursorScreenPos(ImVec2(sp.x + width - rs.x, sp.y));
              ImGui::TextColored(kMuted, "%s", rightText);
          }
          ImGui::Dummy(ImVec2(0, 2));
        };

        // ── MONITORING ──
        char pwrStr[32] = {0};
        float totPwr = hal.GetTotalPower();
        if (totPwr > 0) sprintf(pwrStr, "%.0f W", totPwr);
        SectionStart("MONITORING", pwrStr);
        float cT = hal.GetCpuTemp(), gT = hal.GetGpuTemp();
        auto &oc2 = FanService::Get().GetOverlayConfig();
        const ImU32 cTC =
            ImGui::GetColorU32(TempColor(cT, oc2.cpuWarn, oc2.cpuCrit));
        const ImU32 gTC =
            ImGui::GetColorU32(TempColor(gT, oc2.gpuWarn, oc2.gpuCrit));

        auto readUI = [&](ImDrawList *dl, const char *label, float temp,
                          ImU32 tCol, float loadPct) {
          ImVec2 cp = ImGui::GetCursorScreenPos();
          const float rowH = 28.0f;
          dl->AddRectFilled(cp, ImVec2(cp.x + width, cp.y + rowH),
                            ImGui::GetColorU32(kSurface), 3.0f);
          dl->AddRect(cp, ImVec2(cp.x + width, cp.y + rowH), kBorderU32, 3.0f);

          // Small icon (left)
          if (label[0] == 'C')
            DrawCpuIcon(dl, ImVec2(cp.x + 5, cp.y + 5), 14.0f,
                        ImGui::GetColorU32(kMuted));
          else if (label[0] == 'G')
            DrawGpuIcon(dl, ImVec2(cp.x + 5, cp.y + 5), 14.0f,
                        ImGui::GetColorU32(kMuted));
          else
            DrawDiskIcon(dl, ImVec2(cp.x + 5, cp.y + 5), 14.0f,
                         ImGui::GetColorU32(kMuted));

          // Temperature text (right-aligned, rendered FIRST to know its width)
          char tBuf[16];
          sprintf(tBuf, "%.0f C", temp);  // Fallback to simple C as default font lacks degree symbol
          ImVec2 tSz = ImGui::CalcTextSize(tBuf);
          float tempX = cp.x + width - tSz.x - 5;
          dl->AddText(ImVec2(tempX, cp.y + 7), tCol, tBuf);

          // Load bar + % (between icon and temp, fixed margins)
          if (loadPct >= 0) {
            // Short label above load bar: "CPU" / "GPU"
            char shortName[16] = {};
            const char *sp = strchr(label, ' ');
            if (sp) strncpy(shortName, label, (int)(sp - label));
            else    strncpy(shortName, label, 15);
            
            // Draw the short label right next to the icon
            dl->AddText(ImVec2(cp.x + 24, cp.y + 7), ImGui::GetColorU32(kText), shortName);
            float shortW = ImGui::CalcTextSize(shortName).x;

            // Load% text right of load bar, left of temp
            char lBuf[8];
            sprintf(lBuf, "%.0f%%", loadPct);
            ImVec2 lSz = ImGui::CalcTextSize(lBuf);
            float loadPctX = tempX - lSz.x - 4;
            dl->AddText(ImVec2(loadPctX, cp.y + 7), ImGui::GetColorU32(kMuted), lBuf);

            // Load bar fills space exactly between SHORT NAME and LOAD%
            float barLeft = cp.x + 24 + shortW + 8;
            float barRight = loadPctX - 4;
            float barY = cp.y + rowH * 0.5f;
            if (barRight > barLeft + 4) {
              float barW = barRight - barLeft;
              dl->AddLine(ImVec2(barLeft, barY), ImVec2(barRight, barY),
                          IM_COL32(255, 255, 255, 18), 2.0f);
              if (loadPct > 0)
                dl->AddLine(ImVec2(barLeft, barY),
                            ImVec2(barLeft + barW * (loadPct / 100.f), barY),
                            kAccentU32, 2.0f);
            }
          } else {
            // No load bar -> show label on top row
            // Truncate label to fit between icon and temp to prevent ANY overlap
            float availW = tempX - (cp.x + 24) - 4; // 4px padding
            ImVec2 lbSz = ImGui::CalcTextSize(label);
            if (lbSz.x > availW) {
              // Brute-force substring if too wide (avoids overlap guarantees readability)
              char trunc[64] = {};
              strncpy(trunc, label, 63);
              for (int i = (int)strlen(trunc); i > 0; i--) {
                trunc[i] = '\0';
                if (ImGui::CalcTextSize(trunc).x <= availW - 10) {
                  strcat(trunc, "..");
                  break;
                }
              }
              dl->AddText(ImVec2(cp.x + 24, cp.y + 7), ImGui::GetColorU32(kText), trunc);
            } else {
              dl->AddText(ImVec2(cp.x + 24, cp.y + 7), ImGui::GetColorU32(kText), label);
            }
          }

          ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y + rowH + 2));
        };
        readUI(wdl, "CPU Package", cT, cTC, hal.GetCpuLoad());
        readUI(wdl, "GPU Core", gT, gTC, hal.GetGpuLoad());

        auto &drives = hal.GetDriveTemps();
        for (const auto &dInfo : drives) {
          float dTC = ImGui::GetColorU32(
              TempColor(dInfo.Temperature, oc2.diskWarn, oc2.diskCrit));
          readUI(wdl, dInfo.Model.c_str(), dInfo.Temperature, dTC, -1.0f);
        }

        ImGui::Dummy(ImVec2(0, 2));

        // ── COOLING ──
        SectionStart("COOLING");
        FanControlMode mode = (FanControlMode)hal.GetFanControlMode();

        auto fUI = [&](int idx, const char *lbl, int rpm, float angle,
                       float temp) {
          static FanControlMode lastMode = FanControlMode::Auto;
          const float rowH = 24.0f;
          ImVec2 p = ImGui::GetCursorScreenPos();
          wdl->AddRectFilled(p, ImVec2(p.x + width, p.y + rowH),
                             ImGui::GetColorU32(kSurface), 3.0f);
          wdl->AddRect(p, ImVec2(p.x + width, p.y + rowH), kBorderU32, 3.0f);

          DrawFanModern(wdl, ImVec2(p.x + 14, p.y + rowH*0.5f), 8.0f, angle,
                        rpm > 0 ? kAccentU32 : ImGui::GetColorU32(kMuted));
          wdl->AddText(ImVec2(p.x + 28, p.y + 5), ImGui::GetColorU32(kText), lbl);

          char b[32];
          sprintf(b, "%d RPM", rpm);
          ImVec2 bSz = ImGui::CalcTextSize(b);
          wdl->AddText(ImVec2(p.x + width - bSz.x - 6, p.y + 5),
                       ImGui::GetColorU32(kMuted), b);

          if (mode == FanControlMode::Manual) {
            static int manualTargets[2] = {-1, -1};
            if (lastMode != FanControlMode::Manual) {
              manualTargets[0] = (int)hal.GetFanPercentage(0);
              manualTargets[1] = (int)hal.GetFanPercentage(1);
            }
            ImGui::SetCursorScreenPos(
                ImVec2(p.x + width*0.4f, p.y + 3));
            ImGui::SetNextItemWidth(width * 0.35f);
            ImGui::PushID(idx);
            if (ImGui::SliderInt("##pct", &manualTargets[idx], 0, 100, "%d%%"))
              ; // drag only — apply on release
            if (ImGui::IsItemDeactivatedAfterEdit())
              hal.SetFanSpeed(idx, manualTargets[idx]);
            ImGui::PopID();
          }
          lastMode = mode;
          ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 2));
        };

        static float f1a = 0, f2a = 0;
        f1a += (hal.GetFanSpeed(0) / 1000.0f) * 0.1f;
        f2a += (hal.GetFanSpeed(1) / 1000.0f) * 0.1f;
        fUI(0, "CPU Fan", hal.GetFanSpeed(0), f1a, cT);
        fUI(1, "GPU Fan", hal.GetFanSpeed(1), f2a, gT);

        SectionStart("FAN MODE");
        // Use outer btnH (22px compact)
        float bW3 = (width - 8) / 3.0f;
        auto mBtn = [&](const char *l, FanControlMode m, float w) {
          bool act = (mode == m);
          if (act) {
            ImGui::PushStyleColor(ImGuiCol_Button,        kAccentDim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1,1,1,1));
          } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        kSurface);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kElevated);
            ImGui::PushStyleColor(ImGuiCol_Text,          kMuted);
          }
          if (ImGui::Button(l, ImVec2(w, btnH))) {
            if (m == FanControlMode::Auto) hal.SetFanAuto();
            else hal.SetFanControlMode((int)m);
          }
          ImGui::PopStyleColor(3);
          ImGui::SameLine(0, 4);
        };
        mBtn("Auto",   FanControlMode::Auto,      bW3);
        mBtn("Manual", FanControlMode::Manual,    bW3);
        mBtn("Sync",   FanControlMode::Sync,      bW3);
        ImGui::NewLine();
        float bW2 = (width - 4) / 2.0f;
        mBtn("Optimized", FanControlMode::Optimized, bW2);
        mBtn("Separated", FanControlMode::Separated, bW2);
        ImGui::NewLine();

        ImGui::EndTabItem();
      }

      // ── FAN CURVE TAB ────────────────────────────────────────────────
      if (ImGui::BeginTabItem("Fan Curve")) {
        ImGui::Dummy(ImVec2(0, 8));
        FanControlMode cm = (FanControlMode)hal.GetFanControlMode();
        if (cm != FanControlMode::Auto && cm != FanControlMode::Manual) {
          ImVec2 sp = ImGui::GetCursorScreenPos();
          float aw = ImGui::GetContentRegionAvail().x;
          float ah = 200.0f; // Minimal height for fan curve graph
          float gl = 40, gb = 30;
          ImVec2 cp(sp.x + gl, sp.y);
          ImVec2 cs(aw - gl - 10, ah - gb - 10);
          ImGui::Dummy(ImVec2(aw, ah));

          ImDrawList *dl = ImGui::GetWindowDrawList();
          dl->AddRectFilled(ImVec2(cp.x - 2, cp.y - 2),
                            ImVec2(cp.x + cs.x + 2, cp.y + cs.y + 2),
                            IM_COL32(13, 13, 18, 255), 4.0f);
          dl->AddRect(ImVec2(cp.x - 2, cp.y - 2),
                      ImVec2(cp.x + cs.x + 2, cp.y + cs.y + 2), kBorderU32,
                      4.0f);

          // Grid and Labels (Reduced Y labels to avoid overlap)
          for (int i = 0; i <= 10; i += 2) {
            float y = cp.y + cs.y - (cs.y * (float)i / 10.0f);
            char b[16];
            sprintf(b, "%d%%", i * 10);
            ImVec2 txSz = ImGui::CalcTextSize(b);
            dl->AddText(ImVec2(cp.x - txSz.x - 5, y - 7),
                        ImGui::GetColorU32(kMuted), b);
            dl->AddLine(ImVec2(cp.x, y), ImVec2(cp.x + cs.x, y),
                        IM_COL32(255, 255, 255, 15));
          }
          for (int i = 1; i <= 10; i += 2) { // Unlabeled grid lines
            float y = cp.y + cs.y - (cs.y * (float)i / 10.0f);
            dl->AddLine(ImVec2(cp.x, y), ImVec2(cp.x + cs.x, y),
                        IM_COL32(255, 255, 255, 10));
          }
          for (int i = 0; i <= 10; i++) {
            float x = cp.x + (cs.x * i / 10.0f);
            char b[16];
            sprintf(b, "%d", 40 + i * 5);
            dl->AddText(ImVec2(x - 8, cp.y + cs.y + 4),
                        ImGui::GetColorU32(kMuted), b);
            dl->AddLine(ImVec2(x, cp.y), ImVec2(x, cp.y + cs.y),
                        IM_COL32(255, 255, 255, 15));
          }

          auto DrawC = [&](const char *id, CurvePoint *pts, ImU32 c,
                           float curTemp, float curSpeed) {
            ImVec2 pp;
            for (int i = 0; i < 5; i++) {
              ImVec2 cur(cp.x + ((pts[i].temp - 40.0f) / 50.0f) * cs.x,
                         cp.y + (1.0f - pts[i].speed / 100.0f) * cs.y);
              if (i > 0)
                dl->AddLine(pp, cur, c, 2.5f);
              dl->AddCircleFilled(cur, 5.0f, c);

              ImVec2 mp = ImGui::GetIO().MousePos;
              if ((mp.x - cur.x) * (mp.x - cur.x) +
                      (mp.y - cur.y) * (mp.y - cur.y) <
                  100) {
                dl->AddCircle(cur, 8.0f, IM_COL32(255, 255, 255, 255));
                char tb[32];
                sprintf(tb, "%d C / %d%%", pts[i].temp, pts[i].speed);
                dl->AddText(ImVec2(cur.x + 10, cur.y - 10),
                            IM_COL32(255, 255, 255, 255), tb);
                if (ImGui::IsMouseDown(0) && m_draggingIdx == -1) {
                  m_draggingIdx = i;
                  m_draggingId = id;
                }
              }
              if (m_draggingIdx == i && m_draggingId &&
                  strcmp(m_draggingId, id) == 0) {
                if (ImGui::IsMouseDown(0)) {
                  float nx = 40.0f + ((mp.x - cp.x) / cs.x) * 50.0f;
                  float ny = 100.0f - ((mp.y - cp.y) / cs.y) * 100.0f;
                  pts[i].temp = (int)std::clamp(std::round(nx), 40.0f, 90.0f);
                  pts[i].speed = (int)std::clamp(std::round(ny), 0.0f, 100.0f);
                  if (i > 0 && pts[i].temp <= pts[i - 1].temp)
                    pts[i].temp = pts[i - 1].temp + 1;
                  if (i < 4 && pts[i].temp >= pts[i + 1].temp)
                    pts[i].temp = pts[i + 1].temp - 1;
                } else {
                  pts[i].temp = (int)(std::round(pts[i].temp / 5.0f) * 5.0f);
                  pts[i].speed =
                      (int)(std::round(pts[i].speed / 10.0f) * 10.0f);
                  m_draggingIdx = -1;
                  m_draggingId = nullptr;
                  hal.SetFanControlMode((int)cm);
                }
              }
              pp = cur;
            }
            float cx =
                cp.x +
                ((std::clamp(curTemp, 40.0f, 90.0f) - 40.0f) / 50.0f) * cs.x;
            float cy =
                cp.y +
                (1.0f - std::clamp(curSpeed, 0.0f, 100.0f) / 100.0f) * cs.y;
            dl->AddCircleFilled(ImVec2(cx, cy), 6.0f,
                                IM_COL32(255, 255, 255, 255));
          };

          // Calculate width to center the legend perfectly
          float legendW = 10 + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("CPU").x +
                          ImGui::GetStyle().ItemSpacing.x + 10 + ImGui::GetStyle().ItemSpacing.x + 
                          ImGui::CalcTextSize("GPU").x;
          ImGui::SetCursorPosX((width - legendW) * 0.5f);
          
          ImGui::ColorButton(
              "##cpu_l", ImVec4(kAccent.x, kAccent.y, kAccent.z, 1.0f),
              ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
              ImVec2(10, 10));
          ImGui::SameLine();
          ImGui::Text("CPU");
          ImGui::SameLine();
          ImGui::ColorButton(
              "##gpu_l", ImVec4(kGreen.x, kGreen.y, kGreen.z, 1.0f),
              ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
              ImVec2(10, 10));
          ImGui::SameLine();
          ImGui::Text("GPU");
          ImGui::Dummy(ImVec2(0, 4)); // Reduced blank space
          if (cm == FanControlMode::Separated) {
            DrawC("g", (CurvePoint *)hal.GetGpuCurve(),
                  ImGui::GetColorU32(kGreen), hal.GetGpuTemp(),
                  hal.GetFanSpeed(1) / 1000.0f * 20.0f);
            DrawC("c", (CurvePoint *)hal.GetCpuCurve(), kAccentU32,
                  hal.GetCpuTemp(), hal.GetFanSpeed(0) / 1000.0f * 20.0f);
          } else {
            DrawC("s", (CurvePoint *)hal.GetCpuCurve(), kAccentU32,
                  hal.GetCpuTemp(), hal.GetFanSpeed(0) / 50.0f);
          }
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
          ImGui::TextWrapped("Switch to Manual or Optimized mode in the "
                             "Dashboard tab to edit fan curves.");
          ImGui::PopStyleColor();
        }
        ImGui::EndTabItem();
      }

      // ── OPTIONS TAB ──────────────────────────────────────────────────
      static bool optionsWasOpen = false;
      bool optionsIsOpen = ImGui::BeginTabItem("Options");
      if (optionsIsOpen) {
        // Refresh hardware states when tab is first opened
        static int batLimit = 100;
        static PowerControl::GpuOverclockSettings ocSettings;

        if (!optionsWasOpen) {
             ocSettings = hal.GetGpuOverclockSettings();
        }
        optionsWasOpen = true;

        ImGui::Dummy(ImVec2(0, 4));

        float bw = (width - 16) / 3.0f;
        float btnH = 28.0f;

        ImGui::TextColored(kMuted, "Power Mode");
        int cm = hal.GetPowerMode();
        auto hBtn = [&](const char *l, int v, ImVec4 c) {
          if (cm == v)
            ImGui::PushStyleColor(ImGuiCol_Button, c);
          if (ImGui::Button(l, ImVec2(bw, btnH)))
            hal.SetPowerMode(v);
          if (cm == v)
            ImGui::PopStyleColor();
          ImGui::SameLine(0, 4);
        };
        hBtn("Eco", 0,
             ImVec4(kGreen.x * 0.7f, kGreen.y * 0.7f, kGreen.z * 0.7f, 1.f));
        hBtn("Balanced", 1, ImVec4(0.2f, 0.3f, 0.6f, 1.f));
        hBtn("Turbo", 2,
             ImVec4(kRed.x * 0.7f, kRed.y * 0.7f, kRed.z * 0.7f, 1.f));
        ImGui::NewLine();

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(kMuted, "GPU Mode");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(kRed.x, kRed.y, kRed.z, 0.7f), "(requires reboot)");
        int gm = hal.GetGpuModeInt();
        auto gBtn = [&](const char *l, int v, ImVec4 c) {
          if (gm == v)
            ImGui::PushStyleColor(ImGuiCol_Button, c);
          if (ImGui::Button(l, ImVec2(bw, btnH)))
            hal.RequestGpuMode(v);
          if (gm == v)
            ImGui::PopStyleColor();
          ImGui::SameLine(0, 4);
        };
        gBtn("Hybrid", 0, ImVec4(0.2f, 0.5f, 0.3f, 1.f));
        gBtn("Discrete", 1,
             ImVec4(kRed.x * 0.7f, kRed.y * 0.7f, kRed.z * 0.7f, 1.f));
        gBtn("Integrated", 2, ImVec4(0.2f, 0.4f, 0.6f, 1.f));
        ImGui::NewLine();

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(kMuted, "CPU Undervolting");
        bool isAmd = hal.GetCpuName().find("AMD") != std::string::npos ||
                     hal.GetCpuName().find("Ryzen") != std::string::npos;

        if (isAmd) {
          static int coOff = 0; // Curve Optimizer
          ImGui::Text("All-Core CO:");
          ImGui::SameLine();
          ImGui::TextDisabled("(?)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "AMD Curve Optimizer:\n"
                "- 1 count is approx 3-5mV\n"
                "- Higher negative values = lower voltage\n"
                "- Rec: Ryzen 5000/6000/7000 usually run well at -15 to -20\n"
                "- Always test stability under load (e.g. Cinebench)\n"
                "- 0 is Default (no undervolt)");
          }

          ImGui::SetNextItemWidth(-76); // Stretches leaving 76px for SameLine button
          ImGui::SliderInt("##AmdCO", &coOff, -30, 0, "%d counts");
          ImGui::SameLine();
          if (ImGui::Button("SET##amd", ImVec2(70, 22))) {
            hal.SetAmdCurveOptimizer(coOff);
          }
          ImGui::TextDisabled("Start at -15, test stability, then lower if stable.");
        } else {
          static int coreMv = hal.GetCpuCoreOffset();
          static int cacheMv = hal.GetCpuCacheOffset();
          ImGui::Text("Core Offset:");
          ImGui::SameLine();
          ImGui::TextDisabled("(?)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Intel Undervolting:\n"
                "- Most 10th-13th Gen chips handle -50mV to -100mV well.\n"
                "- Start at -50mV and test stability.\n"
                "- Keep Cache Offset identical to Core for best stability.");
          }
          ImGui::SameLine(110);
          ImGui::SetNextItemWidth(-76);
          if (ImGui::SliderInt("##CoreOff", &coreMv, -150, 0, "%d mV")) {
          }
          ImGui::SameLine();
          if (ImGui::Button("SET##intel", ImVec2(70, 22))) {
            hal.SetCpuUndervolt(coreMv, cacheMv);
          }
          ImGui::Text("Cache Offset:");
          ImGui::SameLine(110);
          ImGui::SetNextItemWidth(-1); // No button, stretch to very end
          ImGui::SliderInt("##CacheOff", &cacheMv, -150, 0, "%d mV");
        }

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(kMuted, "Battery Care");
        
        static int localBatLimit = 0;
        static int realBatLimit = -1;
        if (realBatLimit == -1) {
            realBatLimit = hal.GetBatteryChargeLimit();
        }
        // Self-correct if WMI populates delayed
        if (localBatLimit == 0 || (localBatLimit == 100 && realBatLimit < 100)) {
           localBatLimit = realBatLimit;
        }

        ImGui::SetNextItemWidth(-76);
        if (ImGui::SliderInt("##ChargeLimit", &localBatLimit, 60, 100, "%d%%")) {
          // just dragging, button applies it
        }
        ImGui::SameLine();
        if (ImGui::Button("APPLY##batt", ImVec2(70, 22))) {
          hal.SetBatteryChargeLimit(localBatLimit);
          realBatLimit = localBatLimit; // Update cache internally
        }

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(kMuted, "System");
        bool currentAuto = IsAutoStartEnabled();
        if (ImGui::Checkbox("Run on boot", &currentAuto))
          SetAutoStart(currentAuto);
        ImGui::SameLine(width / 2);
        if (ImGui::Checkbox("HUD Overlay", &m_showOverlayHUD))
          SyncConfig();

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(kMuted, "GPU Overclocking");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "NVIDIA Laptop GPU Tuning:\n"
              "- Core Clock: +100MHz to +150MHz is generally stable.\n"
              "- Memory Clock: +200MHz to +400MHz is typical.\n"
              "- Power Limit: Laptop GPUs are hard-locked by the BIOS. Increasing\n"
              "  this slider often has absolutely no effect unless you flashed an\n"
              "  unlocked vBIOS.");
        }
        if (ocSettings.isSupported) {
          static PowerControl::GpuOverclockSettings ocLocal = ocSettings;
          static bool ocInited = false;
          if (!ocInited) {
            ocLocal = ocSettings;
            ocInited = true;
          }

          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 4));
          ImGui::TextColored(kMuted, "Core Clock Offset");
          ImGui::SetNextItemWidth(-1);
          if (ImGui::SliderInt("##CoreClock", &ocLocal.coreClockOffset,
                           ocSettings.coreMin, ocSettings.coreMax, "%d MHz")) {
            ocLocal.coreClockOffset = (int)std::round(ocLocal.coreClockOffset / 15.0f) * 15;
          }

          ImGui::TextColored(kMuted, "Memory Clock Offset");
          ImGui::SetNextItemWidth(-1);
          if (ImGui::SliderInt("##MemoryClock", &ocLocal.memoryClockOffset,
                           ocSettings.memMin, ocSettings.memMax, "%d MHz")) {
            ocLocal.memoryClockOffset = (int)std::round(ocLocal.memoryClockOffset / 5.0f) * 5;
          }

          ImGui::TextColored(kMuted, "Power Limit");
          ImGui::SetNextItemWidth(-1);
          ImGui::SliderInt("##PwrLmt", &ocLocal.powerLimitPercent,
                           ocSettings.pwrMin, ocSettings.pwrMax, "%d%%");

          ImGui::Dummy(ImVec2(0, 4));
          if (ImGui::Button("APPLY GPU OC", ImVec2(-1, 26))) {
            hal.SetGpuOverclock(ocLocal);
            ocSettings = hal.GetGpuOverclockSettings();
            ocLocal = ocSettings;
          }
          ImGui::PopStyleVar();
        } else {
          ImGui::TextColored(kMuted, "NVIDIA GPU not detected/supported.");
        }
        ImGui::Dummy(ImVec2(0, 8)); // Extra bottom space

        ImGui::EndTabItem();
      } else {
        optionsWasOpen = false;
      }

      ImGui::EndTabBar();
    }

    ImGui::PopStyleVar(); // ItemSpacing

    // Capture content height for OS window auto-resize next frame
    s_lastContentH = ImGui::GetCursorPosY() + style.WindowPadding.y;

    ImGui::End();
  }
  // 4. OVERLAY HUD
  if (m_showOverlayHUD) {
    ImGuiWindowFlags hudFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;
    if (!m_hudResizable)
      hudFlags |= ImGuiWindowFlags_NoResize;

    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::SetNextWindowSize(m_hudSize, ImGuiCond_FirstUseEver);

    if (m_hudPos.x > 0)
      ImGui::SetNextWindowPos(m_hudPos, ImGuiCond_FirstUseEver);

    // Apply global alpha for the whole HUD window
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, m_hudOpacity);
    bool hudOpen = ImGui::Begin("OverlayHUD", &m_showOverlayHUD, hudFlags);
    // PopStyleVar MUST always be called (before or after End, but balanced)
    ImGui::PopStyleVar();

    if (hudOpen) {
      if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        if (ImGuiViewport *vp = ImGui::GetWindowViewport()) {
          if (HWND hwnd = (HWND)vp->PlatformHandle) {
            // Remove WS_EX_APPWINDOW (ImGui backend adds it → forces taskbar)
            // Add   WS_EX_TOOLWINDOW (hides from taskbar & Alt-Tab)
            // Add   WS_EX_LAYERED    (required for SetLayeredWindowAttributes)
            LONG exStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
            LONG newStyle =
                (exStyle | WS_EX_TOOLWINDOW | WS_EX_LAYERED) & ~WS_EX_APPWINDOW;
            if (exStyle != newStyle) {
              SetWindowLong(hwnd, GWL_EXSTYLE, newStyle);
              // SWP_FRAMECHANGED forces Windows to re-evaluate taskbar state
              SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                               SWP_FRAMECHANGED | SWP_NOACTIVATE);
            }

            // Color-key: black pixels become transparent (overlay background)
            // LWA_ALPHA: whole-window opacity, driven by Transparency slider
            BYTE alpha = (BYTE)(m_hudOpacity * 255.0f);
            SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), alpha,
                                       LWA_COLORKEY | LWA_ALPHA);

            // Always-on-top
            bool isTop =
                (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            if (m_hudAlwaysOnTop && !isTop)
              SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            else if (!m_hudAlwaysOnTop && isTop)
              SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          }
        }
      }

      // Track window position changes for config persistence
      m_hudSize = ImGui::GetWindowSize();
      ImVec2 curPos = ImGui::GetWindowPos();

      // Screen border magnetic snapping/clamping
      float snapMargin = 10.0f;
      float screenW = (float)GetSystemMetrics(SM_CXSCREEN);
      float screenH = (float)GetSystemMetrics(SM_CYSCREEN);

      if (curPos.x < snapMargin)
        curPos.x = 0;
      if (curPos.y < snapMargin)
        curPos.y = 0;
      if (curPos.x + m_hudSize.x > screenW - snapMargin)
        curPos.x = screenW - m_hudSize.x;
      if (curPos.y + m_hudSize.y > screenH - snapMargin)
        curPos.y = screenH - m_hudSize.y;

      if (curPos.x != ImGui::GetWindowPos().x ||
          curPos.y != ImGui::GetWindowPos().y) {
        ImGui::SetWindowPos(curPos);
      }

      if (std::abs(curPos.x - m_hudPos.x) > 0.5f ||
          std::abs(curPos.y - m_hudPos.y) > 0.5f) {
        m_hudPos = curPos;
        SyncConfig();
      }

      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 ws = ImGui::GetWindowSize();
      ImVec2 wp = ImGui::GetWindowPos();

      // Read temperatures every frame (no throttle)
      float cCpu = hal.GetCpuTemp();
      float cGpu = hal.GetGpuTemp();
      float lCpu = hal.GetCpuLoad();
      float lGpu = hal.GetGpuLoad();
      auto &oc2 = FanService::Get().GetOverlayConfig();

      // Custom Backdrop: Premium sleek dark translucent finish
      dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(14, 14, 18, 220), 8.0f);
      dl->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(255, 255, 255, 25), 8.0f, 0, 1.0f);

      // Unified drawing function capable of perfectly auto-scaling its contents
      auto DrawPixelComponent = [&](ImVec2 pos, ImVec2 size, const char* name, float temp, float load, float warnTemp, float critTemp) {
          ImFont* font = ImGui::GetFont();
          
          // Palette and String Formatting
          ImU32 tempCol = IM_COL32(50, 240, 180, 255); 
          if (temp >= critTemp) tempCol = IM_COL32(255, 80, 80, 255);
          else if (temp >= warnTemp) tempCol = IM_COL32(255, 180, 0, 255);
          char tBuf[16]; sprintf(tBuf, "%.0f C", temp);
          
          // DYNAMIC RESPONSIVE SCALING: Grow with height, but protect width
          float scale = size.y / 28.0f; 
          float fsz = ImGui::GetFontSize() * scale;
          
          ImVec2 nSz = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, name);
          ImVec2 tSz = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, tBuf);
          float gap = 12.0f * scale; // Tighter gap between label and value
          float totalTextW = nSz.x + tSz.x + gap;

          if (totalTextW > size.x * 0.9f) {
              float adjust = (size.x * 0.9f) / totalTextW;
              scale *= adjust;
              fsz *= adjust;
              nSz = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, name);
              tSz = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, tBuf);
              gap *= adjust;
              totalTextW = nSz.x + tSz.x + gap;
          }

          auto AddSubtleShadowText = [&](ImVec2 tpos, ImU32 col, const char* text) {
              float d = 1.0f * scale;
              // Two passes at soft alpha for a professional lift effect
              dl->AddText(font, fsz, ImVec2(tpos.x+d, tpos.y+d), IM_COL32(0, 0, 0, 140), text);
              dl->AddText(font, fsz, ImVec2(tpos.x+0.5f*d, tpos.y+0.5f*d), IM_COL32(0, 0, 0, 80), text);
              dl->AddText(font, fsz, tpos, col, text); 
          };

          // BLOOM GLOW: Temperature gets the same lightsaber-glow treatment
          auto AddGlowingText = [&](ImVec2 tpos, ImU32 col, const char* text) {
              float d = 1.0f * scale;
              ImU32 bloom = (col & 0x00FFFFFF) | 0x45000000;
              dl->AddText(font, fsz, ImVec2(tpos.x-d, tpos.y), bloom, text);
              dl->AddText(font, fsz, ImVec2(tpos.x+d, tpos.y), bloom, text);
              dl->AddText(font, fsz, ImVec2(tpos.x, tpos.y-d), bloom, text);
              dl->AddText(font, fsz, ImVec2(tpos.x, tpos.y+d), bloom, text);
              AddSubtleShadowText(tpos, col, text);
          };

          // --- ROW 1: TEXT (Centered for tighter look) ---
          float row1Y = pos.y + (size.y * 0.05f);
          float textStartX = pos.x + (size.x - totalTextW) * 0.5f;
          
          AddSubtleShadowText(ImVec2(textStartX, row1Y), IM_COL32(255, 255, 255, 255), name);
          AddGlowingText(ImVec2(textStartX + nSz.x + gap, row1Y), tempCol, tBuf); 


          // --- ROW 2: LIGHTSABER LOAD BAR ---
          int filledSegs = (int)(load / 10.0f + 0.5f);
          if (filledSegs > 10) filledSegs = 10;
          if (load > 0 && filledSegs == 0) filledSegs = 1;

          float lineH = std::max(4.5f * scale, 4.0f); // Thicker for a solid "blade" feel
          float lineW = totalTextW;                 // Perfectly scale length to match the text layout
          float segSpacing = 1.0f;                  // Minimal gap for a continuous bar look
          float segW = (lineW - (segSpacing * 9.0f)) / 10.0f;
          
          float lineX = textStartX;                 // Precise alignment with the text row
          float lineY = row1Y + nSz.y + 6.0f * scale;

          // 1. Draw solid track background (the "handle/blade housing")
          dl->AddRectFilled(ImVec2(lineX, lineY), ImVec2(lineX + lineW, lineY + lineH), IM_COL32(255, 255, 255, 25), lineH * 0.5f);

          for (int i = 0; i < 10; i++) {
              ImVec2 pMin = ImVec2(lineX + i * (segW + segSpacing), lineY);
              ImVec2 pMax = ImVec2(pMin.x + segW, pMin.y + lineH);
              
              if (i < filledSegs) {
                  // Core Blade (Solid White) - Only round the outer-most edges of the total bar
                  float rd = (i == 0) ? lineH * 0.5f : (i == filledSegs - 1 ? lineH * 0.5f : 0.0f);
                  dl->AddRectFilled(pMin, pMax, IM_COL32(255, 255, 255, 255), rd);
                  
                  // Bloom Overlay (Additive Glow)
                  float glow = 1.5f * scale;
                  dl->AddRectFilled(ImVec2(pMin.x-glow, pMin.y-glow), ImVec2(pMax.x+glow, pMax.y+glow), IM_COL32(255, 255, 255, 45), rd+glow);
              }
          }
      };


      float pad = 2.0f; // Tighter outer margins
      if (m_hudVertical) {
          ImVec2 cSz = ImVec2(ws.x - pad * 2, (ws.y - pad * 3) / 2.0f);
          DrawPixelComponent(ImVec2(wp.x + pad, wp.y + pad), cSz, "CPU", cCpu, lCpu, oc2.cpuWarn, oc2.cpuCrit);
          DrawPixelComponent(ImVec2(wp.x + pad, wp.y + pad * 2 + cSz.y), cSz, "GPU", cGpu, lGpu, oc2.gpuWarn, oc2.gpuCrit);
      } else {
          ImVec2 cSz = ImVec2((ws.x - pad * 3) / 2.0f, ws.y - pad * 2);
          DrawPixelComponent(ImVec2(wp.x + pad, wp.y + pad), cSz, "CPU", cCpu, lCpu, oc2.cpuWarn, oc2.cpuCrit);
          DrawPixelComponent(ImVec2(wp.x + pad * 2 + cSz.x, wp.y + pad), cSz, "GPU", cGpu, lGpu, oc2.gpuWarn, oc2.gpuCrit);
      }

      // Right-click context menu
      if (ImGui::BeginPopupContextWindow(
              "HUDContext", ImGuiPopupFlags_MouseButtonRight |
                                ImGuiPopupFlags_NoOpenOverExistingPopup)) {
        // Automatically close popup if user clicks on another application
        // entirely, to prevent it getting stuck open.
        if (ImGui::IsWindowAppearing()) {
          ImGui::SetWindowFocus();
        }
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
          ImGui::CloseCurrentPopup();
        }
        if (ImGui::MenuItem("Always on Top", NULL, m_hudAlwaysOnTop)) {
          m_hudAlwaysOnTop = !m_hudAlwaysOnTop;
          SyncConfig();
        }
        if (ImGui::MenuItem("Resizable", NULL, m_hudResizable))
          m_hudResizable = !m_hudResizable;
        if (ImGui::MenuItem(m_hudVertical ? "Horizontal Layout"
                                          : "Vertical Layout")) {
          m_hudVertical = !m_hudVertical;
          SyncConfig();
        }

        // Transparency slider - always computed from current opacity (no stale
        // static)
        ImGui::SetNextItemWidth(100.0f);
        int tr = (int)((1.0f - m_hudOpacity) * 100.0f + 0.5f);
        if (ImGui::SliderInt("Transparency", &tr, 0, 90, "%d%%")) {
          m_hudOpacity = 1.0f - (tr / 100.0f);
          SyncConfig();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Close Overlay")) {
          m_showOverlayHUD = false;
          SyncConfig();
        }
        ImGui::EndPopup();
      }
    }
    ImGui::End();
  }
}
