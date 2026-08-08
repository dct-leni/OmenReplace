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

OmenHal::OmenHal() {}

OmenHal::~OmenHal() { Shutdown(); }

void OmenHal::Shutdown() {
  if (!m_initialized)
    return;

  OmenLog("[OMEN] Shutdown begin\n");
  m_fanControlActive = false;
  m_stopWorker = true;
  m_workerWake.notify_all();

  if (m_workerThread.joinable())
    m_workerThread.join();

  OmenLog("[OMEN] Shutdown restoring fan control\n");
  FanController::Get().RestoreBios();

  OmenLog("[OMEN] Shutdown complete\n");
  m_initialized = false;
}

bool OmenHal::Initialize() {
  if (m_initialized)
    return true;

  OmenLog("[OMEN] HAL initialize begin\n");

  if (!OmenEc::Get().Initialize()) {
    OmenLog("[OMEN] HAL EC initialize failed\n");
    return false;
  }
  OmenLog("[OMEN] HAL EC initialize ok\n");

  // Trigger initial scans in services
  ThermalService::Get();
  FanService::Get();
  PowerControl::Get();

  if (!m_initialized) {
    WmiHelper wmi;
    if (wmi.Initialize()) {
      m_isDesktop = wmi.IsDesktopMode();
      wmi.Cleanup();
    }
  }

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
  OmenLog("[OMEN] HAL worker started\n");

  return true;
}

int OmenHal::GetBatteryChargeLimit() {
  return PowerControl::Get().GetBatteryChargeLimit();
}

bool OmenHal::SetBatteryChargeLimit(int percentage) {
  return PowerControl::Get().SetBatteryChargeLimit(percentage);
}

bool OmenHal::SetAmdCurveOptimizer(int coCounts) {
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
                  "[OMEN] worker COM initialize failed hr=0x%08lx\n",
                  (unsigned long)comResult);
    OmenLog("%s", message);
    m_fanControlReady = false;
  } else {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "[OMEN] worker COM initialize ok hr=0x%08lx\n",
                  (unsigned long)comResult);
    OmenLog("%s", message);
    m_fanControlReady = true;
  }

  if (FanService::Get().GetControlMode() == FanControlMode::AppMode) {
    m_fanControlActive = true;
    FanService::Get().SetProfile(FanService::Get().GetProfile());
  }

  int updateCounter = 0;
  while (!m_stopWorker) {
    auto start = std::chrono::steady_clock::now();

    // Update thermal and fan services every cycle (1 second)
    ThermalService::Get().Update();
    FanService::Get().Update();

    // Update power control less frequently (every 5 seconds)
    // This reduces expensive WMI calls
    // Update power control less frequently (every 10 seconds)
    // This reduces expensive WMI calls
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
  // Performance mode benefits from the aggressive Cool fan profile.
  if (mode == (int)PowerMode::Performance || mode == (int)PowerMode::Turbo) {
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
