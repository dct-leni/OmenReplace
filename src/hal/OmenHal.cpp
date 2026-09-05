#include "OmenHal.h"
#include "FanService.h"
#include "OmenEc.h"
#include "PowerControl.h"
#include "ThermalService.h"
#include "FanController.h"
#include "MemoryService.h"
#include "OmenLog.h"
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <objbase.h>
#include <string>
#include <winreg.h>

OmenHal &OmenHal::Get() {
  static OmenHal instance;
  return instance;
}

// Foreground window covers its whole monitor without a caption style —
// borderless/exclusive fullscreen (games, slideshow apps).
static bool ForegroundFullscreen() {
  HWND fg = GetForegroundWindow();
  if (!fg)
    return false;

  // Exclude desktop wallpaper and taskbar windows
  wchar_t className[64] = {};
  GetClassNameW(fg, className, ARRAYSIZE(className));
  if (wcscmp(className, L"Progman") == 0 ||
      wcscmp(className, L"WorkerW") == 0 ||
      wcscmp(className, L"Shell_TrayWnd") == 0 ||
      wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
    return false;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid == 0 || pid == GetCurrentProcessId())
    return false;

  // Exclude Windows shell / system processes
  HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (hProc) {
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, exePath, &size)) {
      const wchar_t *exeName = wcsrchr(exePath, L'\\');
      if (exeName) {
        exeName++;
        static const wchar_t *kNonGameExes[] = {
            L"explorer.exe", L"dwm.exe", L"SearchHost.exe",
            L"StartMenuExperienceHost.exe", L"ApplicationFrameHost.exe",
            L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
            L"opera.exe", L"opera_gx.exe", L"vivaldi.exe", L"waterfox.exe",
            L"zen.exe", L"vlc.exe", L"mpv.exe", L"discord.exe",
            L"spotify.exe", L"code.exe", L"devenv.exe", L"windowsterminal.exe"
        };
        for (const wchar_t *nonGame : kNonGameExes) {
          if (_wcsicmp(exeName, nonGame) == 0) {
            CloseHandle(hProc);
            return false;
          }
        }
      }
    }
    CloseHandle(hProc);
  }

  HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi = {sizeof(mi)};
  if (!GetMonitorInfoW(mon, &mi))
    return false;
  RECT wr;
  if (!GetWindowRect(fg, &wr))
    return false;
  if (wr.left != mi.rcMonitor.left || wr.top != mi.rcMonitor.top ||
      wr.right != mi.rcMonitor.right || wr.bottom != mi.rcMonitor.bottom)
    return false;
  return (GetWindowLongPtrW(fg, GWL_STYLE) & WS_CAPTION) == 0;
}

OmenHal::OmenHal() {}

OmenHal::~OmenHal() { Shutdown(); }

void OmenHal::Shutdown() {
  static std::atomic<bool> s_shuttingDown{false};
  if (s_shuttingDown.exchange(true))
    return;
  if (!m_initialized)
    return;

  OmenLog("[AMDOMEN] Shutdown begin\n");
  m_fanControlActive = false;
  m_stopWorker = true;
  m_workerWake.notify_all();

  if (m_workerThread.joinable())
    m_workerThread.join();

  OmenLog("[AMDOMEN] Shutdown restoring fan control to BIOS hardware\n");
  FanController::Get().RestoreBios();

  OmenLog("[AMDOMEN] Shutdown complete\n");
  m_initialized = false;
}

bool OmenHal::Initialize() {
  if (m_initialized)
    return true;

  OmenLog("[AMDOMEN] HAL initialize begin\n");

  if (!OmenEc::Get().Initialize()) {
    OmenLog("[AMDOMEN] HAL EC initialize failed\n");
    return false;
  }
  OmenLog("[AMDOMEN] HAL EC initialize ok\n");

  // Trigger initial scans in services
  ThermalService::Get();
  FanService::Get();
  PowerControl::Get();

  {
    char buf[256] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     "ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf,
                     &sz) == ERROR_SUCCESS &&
        sz > 0) {
      m_cpuName = buf;
      while (!m_cpuName.empty() && m_cpuName.back() == ' ')
        m_cpuName.pop_back();
      size_t radeon = m_cpuName.find(" with Radeon");
      if (radeon != std::string::npos) m_cpuName = m_cpuName.substr(0, radeon);
    }
    if (m_cpuName.empty())
      m_cpuName = "Unknown CPU";

    DISPLAY_DEVICEA dd = {sizeof(dd)};
    if (EnumDisplayDevicesA(nullptr, 0, &dd, 0)) {
      m_gpuName = dd.DeviceString;
      const char* prefixes[] = {"NVIDIA GeForce ", "AMD Radeon ", "Intel "};
      for (const char* pfx : prefixes) {
        if (m_gpuName.compare(0, strlen(pfx), pfx) == 0) {
          m_gpuName = m_gpuName.substr(strlen(pfx));
          break;
        }
      }
    }
    if (m_gpuName.empty())
      m_gpuName = "Unknown GPU";
  }

  m_stopWorker = false;
  m_fanControlReady = false;
  m_fanControlActive = false;
  m_initialized = true;
  m_workerThread = std::thread(&OmenHal::BackgroundLoop, this);
  OmenLog("[AMDOMEN] HAL worker started\n");

  return true;
}

int OmenHal::GetBatteryChargeLimit() {
  return PowerControl::Get().GetBatteryChargeLimit();
}

bool OmenHal::SetBatteryChargeLimit(int percentage) {
  return PowerControl::Get().SetBatteryChargeLimit(percentage);
}

bool OmenHal::GetDisplayOverdrive() {
  return PowerControl::Get().GetDisplayOverdrive();
}

bool OmenHal::SetDisplayOverdrive(bool enable) {
  return PowerControl::Get().SetDisplayOverdrive(enable);
}

bool OmenHal::SetAmdCurveOptimizer(int coCounts) {
  FanService::Get().GetOverlayConfig().amdCurveOptimizer = coCounts;
  PowerControl::Get().SetCachedAmdCurveOptimizer(coCounts);
  return PowerControl::Get().SetAmdCurveOptimizer(coCounts);
}

int OmenHal::GetCachedAmdCurveOptimizer() {
  return PowerControl::Get().GetCachedAmdCurveOptimizer();
}

int OmenHal::GetAmdCurveOptimizer() {
  return PowerControl::Get().GetAmdCurveOptimizer();
}

bool OmenHal::SetStapmLimit(int watts) {
  return PowerControl::Get().SetStapmLimit(watts);
}

bool OmenHal::GetPowerThermalLimits(int &powerW, int &tempC) {
  return PowerControl::Get().GetPowerThermalLimits(powerW, tempC);
}

bool OmenHal::SetTctlTemp(int tempC) {
  return PowerControl::Get().SetTctlTemp(tempC);
}

bool OmenHal::GetIsDesktop() { return m_isDesktop; }

bool OmenHal::GetIsAnotherFanControllerActive() {
  return m_anotherFanControllerActive;
}

void OmenHal::BackgroundLoop() {
  HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(comResult)) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "[AMDOMEN] worker COM initialize failed hr=0x%08lx\n",
                  (unsigned long)comResult);
    OmenLog("%s", message);
    m_fanControlReady = false;
  } else {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "[AMDOMEN] worker COM initialize ok hr=0x%08lx\n",
                  (unsigned long)comResult);
    OmenLog("%s", message);
    m_fanControlReady = true;
    PowerControl::Get().InitGpuMux();
    WmiHelper wmi;
    if (wmi.Initialize()) {
      m_isDesktop = wmi.IsDesktopMode();
      wmi.Cleanup();
    }
  }

  // ═════════════════════════════════════════════════════════════════════════
  // STARTUP DISCOVERY & SYNC PIPELINE (First Read, Then Write If Different)
  // ═════════════════════════════════════════════════════════════════════════
  {
    // PHASE 1: Read Actual Hardware States
    PowerMode hwPowerMode = PowerControl::Get().ReadHardwarePowerMode();
    int hwGpuPower = PowerControl::Get().ReadHardwareGpuPower();
    int hwCo = PowerControl::Get().ReadHardwareAmdCurveOptimizer();
    int hwTctl = PowerControl::Get().ReadHardwareTctlLimit();
    int hwBattery = PowerControl::Get().ReadHardwareBatteryLimit();
    bool hwOverdrive = PowerControl::Get().ReadHardwareDisplayOverdrive();
    int hwPowerW = 0, hwTempC = 0;
    PowerControl::Get().GetPowerThermalLimits(hwPowerW, hwTempC);

    OmenLog("[AMDOMEN] Startup HW Discovery: PowerMode=%d, GPU=%d, CO=%d, Tctl=%dC, Battery=%d%%, Overdrive=%d, PPT=%dW\n",
            (int)hwPowerMode, hwGpuPower, hwCo, hwTctl, hwBattery, hwOverdrive ? 1 : 0, hwPowerW);

    // PHASE 2: Compare with Config Targets
    auto &cfg = FanService::Get().GetOverlayConfig();
    PowerMode targetMode = static_cast<PowerMode>(cfg.powerMode);
    if ((int)targetMode > 2) {
      targetMode = PowerMode::Performance;
    }
    int targetGpuPower = cfg.gpuPowerLevel;
    if (targetGpuPower < 0 || targetGpuPower > 2) {
      if (targetMode == PowerMode::Eco) targetGpuPower = 0;
      else if (targetMode == PowerMode::Balanced) targetGpuPower = 1;
      else targetGpuPower = 2;
    }
    int targetCo = cfg.amdCurveOptimizer;
    int targetTctl = cfg.tctlLimit;
    int targetBattery = cfg.batteryLimit;
    bool targetOverdrive = cfg.displayOverdrive;

    // PHASE 3: Write Only If Different (Never spam redundant power schemes)
    if (hwPowerMode != targetMode) {
      OmenLog("[AMDOMEN] PowerMode differs (HW=%d, Cfg=%d) -> Applying target\n",
              (int)hwPowerMode, (int)targetMode);
      PowerControl::Get().SetMode(targetMode);
    } else {
      OmenLog("[AMDOMEN] PowerMode matches target (%d) - no change needed\n", (int)targetMode);
    }

    if (hwGpuPower != targetGpuPower) {
      OmenLog("[AMDOMEN] GPU Power differs (HW=%d, Cfg=%d) -> Applying target\n",
              hwGpuPower, targetGpuPower);
      PowerControl::Get().SetGpuPower((uint8_t)targetGpuPower);
    } else {
      OmenLog("[AMDOMEN] GPU Power matches target (%d) - no change needed\n", targetGpuPower);
    }

    if (targetCo != 0 && hwCo != targetCo) {
      OmenLog("[AMDOMEN] AMD CO differs (HW=%d, Cfg=%d) -> Applying target\n",
              hwCo, targetCo);
      PowerControl::Get().SetAmdCurveOptimizer(targetCo);
    }

    if (targetTctl > 0 && hwTctl != targetTctl) {
      OmenLog("[AMDOMEN] Tctl Limit differs (HW=%dC, Cfg=%dC) -> Applying target\n",
              hwTctl, targetTctl);
      PowerControl::Get().SetTctlTemp(targetTctl);
    }

    if (targetBattery <= 80 && hwBattery != targetBattery) {
      OmenLog("[AMDOMEN] Battery Limit differs (HW=%d%%, Cfg=%d%%) -> Applying target\n",
              hwBattery, targetBattery);
      PowerControl::Get().SetBatteryChargeLimit(targetBattery);
    }

    if (hwOverdrive != targetOverdrive) {
      OmenLog("[AMDOMEN] Display Overdrive differs (HW=%d, Cfg=%d) -> Applying target\n",
              hwOverdrive ? 1 : 0, targetOverdrive ? 1 : 0);
      PowerControl::Get().SetDisplayOverdrive(targetOverdrive);
    }

    if (cfg.wakeOnLan) {
      PowerControl::Get().SetWakeOnLan(true);
    }
    if (cfg.wakeOnWlanBt) {
      PowerControl::Get().SetWakeOnWlanBt(true);
    }
    if (cfg.lowLatencyNetwork) {
      PowerControl::Get().SetNetworkGamingTweak(true);
    }

    if (FanService::Get().GetControlMode() == FanControlMode::AppMode) {
      m_fanControlActive = true;
      FanService::Get().SetProfile(FanService::Get().GetProfile());
      OmenLog("[AMDOMEN] Fan mode active with profile=%d\n", (int)FanService::Get().GetProfile());
    } else {
      OmenLog("[AMDOMEN] Fan mode BIOS Auto\n");
    }

    // PPT Readback Verification Loop (Eco 35/25/25, Balanced 54/45/45, Performance 65/60/55)
    int fast = 0, slow = 0, stapm = 0;
    switch (targetMode) {
    case PowerMode::Eco:         fast = 35; slow = 25; stapm = 25; break;
    case PowerMode::Balanced:    fast = 54; slow = 45; stapm = 45; break;
    case PowerMode::Performance: fast = 65; slow = 60; stapm = 55; break;
    default: break;
    }

    auto diff = [](int a, int b) { return a > b ? a - b : b - a; };

    for (int attempt = 1; fast > 0 && attempt <= 5 && !m_stopWorker; attempt++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      if (m_stopWorker) break;
      int pw = 0, tc = 0;
      if (PowerControl::Get().GetPowerThermalLimits(pw, tc)) {
        if (diff(pw, fast) <= 2 || diff(pw, slow) <= 2 || diff(pw, stapm) <= 2) {
          OmenLog("[AMDOMEN] Startup power limit verified %dW (tctl %dC) on attempt %d\n",
                  pw, tc, attempt);
          break;
        }
      }
      PowerControl::Get().SetMode(targetMode);
    }
  }

  int updateCounter = 0;
  while (!m_stopWorker) {
    auto start = std::chrono::steady_clock::now();

    // Update thermal and fan services every cycle (1 second)
    ThermalService::Get().Update();
    FanService::Get().Update();
    PowerControl::Get().CheckAcLine();

    // ── Game auto-profile ────────────────────────────────────────────────
    // Game = fullscreen foreground + GPU load > 40% sustained 5s.
    // Exit = condition false 10s → restore mode/profile captured at entry.
    static bool gameActive = false;
    static int gameHighSecs = 0, gameLowSecs = 0;
    static PowerMode gameSavedMode = PowerMode::Balanced;
    static int gameSavedProfile = 0;
    static FanControlMode gameSavedControlMode = FanControlMode::Auto;
    static DWORD gameSavedPid = 0;
    static DWORD_PTR gameSavedAffinity = 0;
    static DWORD gameSavedPriority = 0;

    if (FanService::Get().GetOverlayConfig().gameAutoProfile) {
      bool fullscreen = ForegroundFullscreen();
      float gpuLoad = ThermalService::Get().GetGpuLoad();
      if (!gameActive) {
        if (fullscreen && gpuLoad > 40.0f) {
          if (++gameHighSecs >= 5) {
            gameActive = true;
            gameHighSecs = 0;
            gameSavedMode = PowerControl::Get().GetCurrentMode();
            gameSavedControlMode = FanService::Get().GetControlMode();
            gameSavedProfile = (int)FanService::Get().GetProfile();
            OmenLog("[AMDOMEN] game_mode enter (saved mode=%d profile=%d fanMode=%d)\n",
                    (int)gameSavedMode, gameSavedProfile, (int)gameSavedControlMode);
            PowerControl::Get().SetMode(PowerMode::Performance);
            FanService::Get().SetControlMode(FanControlMode::AppMode, false);
            FanService::Get().SetProfile(FanControlProfile::Cool, false);

            // Dual-CCD Core Affinity & OS Gaming Tuning
            HWND fg = GetForegroundWindow();
            if (fg) {
              DWORD pid = 0;
              GetWindowThreadProcessId(fg, &pid);
              if (pid != 0 && pid != GetCurrentProcessId()) {
                HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (hProc) {
                  DWORD_PTR origAff = 0, sysAff = 0;
                  if (GetProcessAffinityMask(hProc, &origAff, &sysAff)) {
                    gameSavedPid = pid;
                    gameSavedAffinity = origAff;
                    gameSavedPriority = GetPriorityClass(hProc);
                    // Ryzen 9 8940HX: Pin to CCD0 (Cores 0..7 / Threads 0..15 -> mask 0x0000FFFF)
                    DWORD_PTR ccd0Mask = 0x0000FFFF;
                    SetProcessAffinityMask(hProc, ccd0Mask);
                    SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS);
                    OmenLog("[AMDOMEN] game_affinity: PID %lu pinned to CCD0 (mask 0x%llx -> 0x%llx)\n",
                            pid, (unsigned long long)origAff, (unsigned long long)ccd0Mask);
                  }
                  CloseHandle(hProc);
                }
              }
            }
            timeBeginPeriod(1); // 1.0ms high-resolution timer
          }
        } else {
          gameHighSecs = 0;
        }
        gameLowSecs = 0;
      } else {
        if (!fullscreen || gpuLoad < 40.0f) {
          if (++gameLowSecs >= 10) {
            gameActive = false;
            gameLowSecs = 0;
            OmenLog("[AMDOMEN] game_mode exit -> restore mode=%d profile=%d fanMode=%d\n",
                    (int)gameSavedMode, gameSavedProfile, (int)gameSavedControlMode);
            PowerControl::Get().SetMode(gameSavedMode);
            if (gameSavedControlMode == FanControlMode::Auto) {
              FanService::Get().SetFanAuto(false);
            } else {
              FanService::Get().SetControlMode(FanControlMode::AppMode, false);
              FanService::Get().SetProfile((FanControlProfile)gameSavedProfile, false);
            }

            if (gameSavedPid != 0 && gameSavedAffinity != 0) {
              HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, gameSavedPid);
              if (hProc) {
                SetProcessAffinityMask(hProc, gameSavedAffinity);
                SetPriorityClass(hProc, gameSavedPriority);
                CloseHandle(hProc);
                OmenLog("[AMDOMEN] game_affinity: PID %lu restored (mask 0x%llx)\n",
                        gameSavedPid, (unsigned long long)gameSavedAffinity);
              }
              gameSavedPid = 0;
              gameSavedAffinity = 0;
            }
            timeEndPeriod(1);
          }
        } else {
          gameLowSecs = 0;
        }
        gameHighSecs = 0;
      }
    } else if (gameActive) {
      gameActive = false;
      PowerControl::Get().SetMode(gameSavedMode);
      if (gameSavedControlMode == FanControlMode::Auto) {
        FanService::Get().SetFanAuto(false);
      } else {
        FanService::Get().SetControlMode(FanControlMode::AppMode, false);
        FanService::Get().SetProfile((FanControlProfile)gameSavedProfile, false);
      }

      if (gameSavedPid != 0 && gameSavedAffinity != 0) {
        HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, gameSavedPid);
        if (hProc) {
          SetProcessAffinityMask(hProc, gameSavedAffinity);
          SetPriorityClass(hProc, gameSavedPriority);
          CloseHandle(hProc);
        }
        gameSavedPid = 0;
        gameSavedAffinity = 0;
      }
      timeEndPeriod(1);
    }

    // ── HUD topmost reassert (fullscreen apps can demote it) ────────────
    static auto lastHudAssert = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    if (FanService::Get().GetOverlayConfig().show && ForegroundFullscreen()) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHudAssert).count() > 2000) {
        lastHudAssert = now;
        HWND hud = FindWindowW(L"AMDOMEN_HUD_WINDOW", nullptr);
        if (hud)
          PostMessageW(hud, WM_APP + 50, 0, 0);
      }
    }

    // Update power control less frequently (every 5 seconds)
    // Update power control every 10 seconds to reduce expensive WMI calls.
    if (updateCounter % 10 == 0) {
      PowerControl::Get().Update();
    }

    updateCounter++;
    if (updateCounter >= 100)
      updateCounter = 0;

    // 2. Poll WMI for system sensors and desktop fan tools
    static auto lastSlowerPoll = std::chrono::steady_clock::now();
    auto loopEnd = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd -
                                                              lastSlowerPoll)
            .count() >= 2000) {
      if (m_isDesktop) {
        WmiHelper wmi;
        if (wmi.Initialize()) {
          m_anotherFanControllerActive = wmi.IsAnotherFanControllerActive();
          wmi.Cleanup();
        }
      }

      lastSlowerPoll = std::chrono::steady_clock::now();
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();

    // Always sleep at least 100ms to prevent CPU hogging if updates are slow
    int sleepTime = (int)(1000 - elapsed);
    if (sleepTime < 100)
      sleepTime = 100;

    std::unique_lock<std::mutex> lock(m_workerWakeMutex);
    m_workerWake.wait_for(lock, std::chrono::milliseconds(sleepTime),
                          [this] { return m_stopWorker.load(); });
  }

  m_fanControlReady = false;
  m_fanControlActive = false;
  if (SUCCEEDED(comResult))
    CoUninitialize();
}

float OmenHal::GetCpuTemp() { return ThermalService::Get().GetCpuTemp(); }
float OmenHal::GetGpuTemp() { return ThermalService::Get().GetGpuTemp(); }
float OmenHal::GetRamTemp() { return ThermalService::Get().GetRamTemp(); }
int OmenHal::GetRamTemps(float &t0, float &t1) {
  return ThermalService::Get().GetRamTemps(t0, t1);
}
float OmenHal::GetTotalPower() { return ThermalService::Get().GetTotalPower(); }
float OmenHal::GetCpuPower() { return ThermalService::Get().GetCpuPower(); }
float OmenHal::GetGpuPower() { return ThermalService::Get().GetGpuPower(); }
float OmenHal::GetCpuLoad() { return ThermalService::Get().GetCpuLoad(); }
float OmenHal::GetGpuLoad() { return ThermalService::Get().GetGpuLoad(); }
const std::vector<DriveInfo> &OmenHal::GetDriveTemps() {
  return ThermalService::Get().GetDriveTemps();
}

float OmenHal::GetFanSpeed(int fanIndex) {
  return FanService::Get().GetFanSpeed(fanIndex);
}
float OmenHal::GetFanPercentage(int fanIndex) {
  return FanService::Get().GetFanPercentage(fanIndex);
}
bool OmenHal::GetDriverStatus() { return OmenEc::Get().IsInitialized(); }
void OmenHal::SetFanAuto() { FanService::Get().SetFanAuto(); }

void OmenHal::SetPowerMode(int mode) {
  PowerControl::Get().SetMode((PowerMode)mode);
  if (mode == (int)PowerMode::Eco) {
    PowerControl::Get().SetGpuPowerOverride(0); // None
  } else if (mode == (int)PowerMode::Balanced) {
    PowerControl::Get().SetGpuPowerOverride(1); // TGP
  } else if (mode == (int)PowerMode::Performance) {
    PowerControl::Get().SetGpuPowerOverride(2); // +Boost
    FanService::Get().SetControlMode(FanControlMode::AppMode);
    FanService::Get().SetProfile(FanControlProfile::Cool);
  }
  FanService::Get().SaveConfig();
}
int OmenHal::GetPowerMode() {
  return (int)PowerControl::Get().GetCurrentMode();
}
std::string OmenHal::GetGpuModeStr() {
  return PowerControl::Get().GetGpuModeStr();
}
int OmenHal::GetGpuModeInt() { return PowerControl::Get().GetGpuModeInt(); }
void OmenHal::RequestGpuMode(int mode) {
  PowerControl::Get().RequestGpuMode(mode);
}

int OmenHal::GetFanControlMode() {
  return (int)FanService::Get().GetControlMode();
}

void OmenHal::SetFanControlMode(int mode) {
  FanService::Get().SetControlMode((FanControlMode)mode);
}

void OmenHal::OptimizeMemory() { MemoryService::Get().Optimize(); }

bool OmenHal::GetWakeOnLan() { return PowerControl::Get().GetWakeOnLan(); }
bool OmenHal::SetWakeOnLan(bool enable) { return PowerControl::Get().SetWakeOnLan(enable); }
bool OmenHal::GetWakeOnWlanBt() { return PowerControl::Get().GetWakeOnWlanBt(); }
bool OmenHal::SetWakeOnWlanBt(bool enable) { return PowerControl::Get().SetWakeOnWlanBt(enable); }

bool OmenHal::GetNetworkGamingTweak() { return PowerControl::Get().GetNetworkGamingTweak(); }
bool OmenHal::SetNetworkGamingTweak(bool enable) { return PowerControl::Get().SetNetworkGamingTweak(enable); }
