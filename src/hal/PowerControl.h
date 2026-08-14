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

  // CPU Undervolting
  int GetCpuCoreOffset();
  int GetCpuCacheOffset();
  bool SetCpuUndervolt(int coreMv, int cacheMv);
  int GetAmdCurveOptimizer();  // Read current all-core CO value from SMU
  bool SetAmdCurveOptimizer(int coCounts);
  int GetCachedAmdCurveOptimizer() { return m_amdCurveOptimizer; }
  void SetCachedAmdCurveOptimizer(int val) { m_amdCurveOptimizer = val; }

  // HP WMI BIOS Power Limits (0x29) & Thermal Policy (0x1A)
  bool SetCpuPowerLimit(int pl1Watts, int pl2Watts);
  bool SetThermalPolicy(uint8_t modeByte);

  // CPU power limits via AMD SMU (Fast, Slow, STAPM PPT)
  bool SetAmdAllPptLimits(int fastW, int slowW, int stapmW);
  bool SetStapmLimit(int watts);
  bool GetPowerThermalLimits(int &powerW, int &tempC);
  bool SetTctlTemp(int tempC);

  // Battery Care
  int GetBatteryChargeLimit();
  bool SetBatteryChargeLimit(int limitPercent);

  // GPU MUX startup probe (WMI 0x52; only changes on reboot, no need to poll)
  void InitGpuMux();

  // Memory & System Optimization
  bool FlushMemoryWorkingSet();
  bool GetSystemRamUsage(float &usedGb, float &totalGb, float &pct);
  float GetCpuVoltage();

  // Windows Power Plan & CPU Boost
  void SetCpuBoostMode(int boostMode);

  bool SetFanLevelWmi(int cpuPercent, int gpuPercent); // WMI Method 0x2E
  bool SetFanLevelWmiBg(
      int cpuPercent,
      int gpuPercent); // Background thread version (uses persistent WMI)

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

  enum class ThermalPolicyVersion { V0_Legacy, V1_Modern, Unknown };
  ThermalPolicyVersion m_thermalPolicy = ThermalPolicyVersion::Unknown;

  // Persistent WMI helper for background thread
  WmiHelper m_wmiBg;
};
