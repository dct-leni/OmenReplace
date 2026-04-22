#include "OmenHal.h"
#include "FanService.h"
#include "OmenEc.h"
#include "PowerControl.h"
#include "ThermalService.h"
#include "MemoryService.h"
#include <chrono>
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <string>

OmenHal &OmenHal::Get() {
  static OmenHal instance;
  return instance;
}

OmenHal::OmenHal() {}

OmenHal::~OmenHal() { Shutdown(); }

void OmenHal::Shutdown() {
  if (!m_initialized)
    return;
  m_stopWorker = true;
  if (m_workerThread.joinable())
    m_workerThread.join();

  // Ensure fans return to BIOS control on app exit without overwriting config
  OmenEc::Get().RestoreAutoControl();
  PowerControl::Get().RestoreFanAuto();
  m_initialized = false;
}

bool OmenHal::Initialize() {
  if (m_initialized)
    return true;

  if (!OmenEc::Get().Initialize())
    return false;

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

  // CPU ID for name
  int cpuInfo[4] = {-1};
  char brand[0x40];
  memset(brand, 0, sizeof(brand));
  __cpuid(cpuInfo, 0x80000000);
  if (cpuInfo[0] >= 0x80000004) {
    __cpuid((int *)(brand), 0x80000002);
    __cpuid((int *)(brand + 16), 0x80000003);
    __cpuid((int *)(brand + 32), 0x80000004);
    m_cpuName = std::string(brand);
    // Cleanup - remove trailing spaces often present in brand string
    m_cpuName.erase(m_cpuName.find_last_not_of(" ") + 1);
  }

  // Detect CPU brand
  if (m_cpuName.find("AMD") != std::string::npos || m_cpuName.find("Ryzen") != std::string::npos) {
      m_cpuBrand = 1; // AMD
  } else if (m_cpuName.find("Intel") != std::string::npos) {
      m_cpuBrand = 2; // Intel
  }

  // Simple GPU name from Nvidia (if avail) or unknown
  m_gpuName = "NVIDIA GPU";

  m_initialized = true;
  m_workerThread = std::thread(&OmenHal::BackgroundLoop, this);

  return true;
}

void OmenHal::Update() {}

int OmenHal::GetBatteryChargeLimit() {
  return PowerControl::Get().GetBatteryChargeLimit();
}

PowerControl::GpuOverclockSettings OmenHal::GetGpuOverclockSettings() {
  return PowerControl::Get().GetGpuOverclock();
}

bool OmenHal::SetGpuOverclock(
    const PowerControl::GpuOverclockSettings &settings) {
  return PowerControl::Get().SetGpuOverclock(settings);
}

bool OmenHal::SetBatteryChargeLimit(int percentage) {
  return PowerControl::Get().SetBatteryChargeLimit(percentage);
}

int OmenHal::GetCpuCoreOffset() { return PowerControl::Get().GetCpuCoreOffset(); }
int OmenHal::GetCpuCacheOffset() { return PowerControl::Get().GetCpuCacheOffset(); }

bool OmenHal::SetCpuUndervolt(int coreMv, int cacheMv) {
  return PowerControl::Get().SetCpuUndervolt(coreMv, cacheMv);
}

bool OmenHal::SetAmdCurveOptimizer(int coCounts) {
  return PowerControl::Get().SetAmdCurveOptimizer(coCounts);
}

int OmenHal::GetCachedAmdCurveOptimizer() {
  return PowerControl::Get().GetCachedAmdCurveOptimizer();
}

void OmenHal::SetCachedAmdCurveOptimizer(int val) {
  PowerControl::Get().SetCachedAmdCurveOptimizer(val);
}

int OmenHal::GetAmdCurveOptimizer() {
  return PowerControl::Get().GetAmdCurveOptimizer();
}

bool OmenHal::GetIsDesktop() { return m_isDesktop; }

bool OmenHal::GetIsAnotherFanControllerActive() {
  return m_anotherFanControllerActive;
}

void OmenHal::BackgroundLoop() {
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

    std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
  }
}

float OmenHal::GetCpuTemp() { return ThermalService::Get().GetCpuTemp(); }
float OmenHal::GetGpuTemp() { return ThermalService::Get().GetGpuTemp(); }
float OmenHal::GetTotalPower() { return ThermalService::Get().GetTotalPower(); }
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
bool OmenHal::GetFanManual() { return FanService::Get().IsManualMode(); }

void OmenHal::SetFanSpeed(int fanIndex, int value) {
  FanService::Get().SetFanSpeed(fanIndex, value);
}
void OmenHal::SetFanAuto() { FanService::Get().SetFanAuto(); }

void OmenHal::SetPowerMode(int mode) {
  PowerControl::Get().SetMode((PowerMode)mode);
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

void *OmenHal::GetCpuCurve() { return (void *)FanService::Get().GetCpuCurve(); }

void *OmenHal::GetGpuCurve() { return (void *)FanService::Get().GetGpuCurve(); }

void OmenHal::OptimizeMemory() { MemoryService::Get().Optimize(); }
