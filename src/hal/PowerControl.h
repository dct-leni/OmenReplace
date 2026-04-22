#pragma once
#include "WmiHelper.h"
#include <mutex>
#include <string>
#include <windows.h>

enum class PowerMode { Eco = 0, Balanced = 1, Performance = 2, Turbo = 3 };

class PowerControl {
public:
  static PowerControl &Get();

  void Update(); // Called from HAL loop
  void SetMode(PowerMode mode);
  void RestoreFanAuto(); // WMI BIOS SetFanMode Default (0x30)
  PowerMode GetCurrentMode();

  // Graphics Mode
  std::string GetGpuModeStr();
  int GetGpuModeInt() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_gpuMode;
  }
  void RequestGpuMode(int mode);

  // GPU Overclocking
  struct GpuOverclockSettings {
    bool isNvidia = true;
    bool isSupported = true;
    int coreClockOffset = 0;
    int memoryClockOffset = 0;
    int powerLimitPercent = 100;

    int coreMin = -150;
    int coreMax = 250;
    int memMin = -500;
    int memMax = 1500;
    int pwrMin = 50;
    int pwrMax = 120;
  };
  GpuOverclockSettings GetGpuOverclock();
  bool SetGpuOverclock(const GpuOverclockSettings &settings);

  // CPU Undervolting
  int GetCpuCoreOffset();
  int GetCpuCacheOffset();
  bool SetCpuUndervolt(int coreMv, int cacheMv);
  int GetAmdCurveOptimizer();  // Read current all-core CO value from SMU
  bool SetAmdCurveOptimizer(int coCounts);
  int GetCachedAmdCurveOptimizer() { return m_amdCurveOptimizer; }
  void SetCachedAmdCurveOptimizer(int val) { m_amdCurveOptimizer = val; }

  // Battery Care
  int GetBatteryChargeLimit();
  bool SetBatteryChargeLimit(int limitPercent);

  bool SetFanLevelWmi(int cpuPercent, int gpuPercent); // WMI Method 0x2E
  bool SetFanLevelWmiBg(
      int cpuPercent,
      int gpuPercent); // Background thread version (uses persistent WMI)
  bool GetFanLevelWmi(int &cpuLevel, int &gpuLevel);
  bool ExtendFanCountdown(); // WMI Heartbeat (0x31)

private:
  PowerControl();

  std::mutex m_mutex;
  PowerMode m_currentMode = PowerMode::Balanced;
  int m_gpuMode = -1; // 0=Hybrid, 1=Discrete, 2=Optimus
  int m_batteryLimitPercent = 100; // Cached battery threshold (WMI only knows on/off)
  int m_amdCurveOptimizer = 0;

  // WMI HP BIOS Helper
  bool CallHpBios(uint32_t cmd, uint32_t type, uint8_t *data, size_t size,
                  size_t expectedOutSize = 0, WmiHelper *pWmi = nullptr);

  // Windows Power Helper
  void SetWindowsPowerPlan(PowerMode mode);

  // Internal helpers
  void CheckThermalPolicy();
  bool SetGpuPower(uint8_t level); // 0=Min, 1=Med, 2=Max
  bool SetGpuMode(int mode);       // 0=Hybrid, 1=Discrete

  enum class ThermalPolicyVersion { V0_Legacy, V1_Modern, Unknown };
  ThermalPolicyVersion m_thermalPolicy = ThermalPolicyVersion::Unknown;

  // Persistent WMI helper for background thread
  WmiHelper m_wmiBg;
};
