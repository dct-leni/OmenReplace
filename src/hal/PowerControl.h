#pragma once
#include "WmiHelper.h"
#include <mutex>
#include <string>
#include <windows.h>

enum class PowerMode { Eco = 0, Balanced = 1, Performance = 2 };

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

  // Hardware State Discovery (First Read)
  PowerMode ReadHardwarePowerMode();
  int ReadHardwareGpuPower();
  int ReadHardwareAmdCurveOptimizer();
  int ReadHardwareTctlLimit();
  int ReadHardwareBatteryLimit();

  // Battery Care
  int GetBatteryChargeLimit();
  bool SetBatteryChargeLimit(int limitPercent);

  // Display Panel Overdrive (LCD Response Time Optimization)
  bool GetDisplayOverdrive();
  bool SetDisplayOverdrive(bool enable);
  bool ReadHardwareDisplayOverdrive();

  // GPU MUX startup probe (WMI 0x52; only changes on reboot, no need to poll)
  void InitGpuMux();

  // GPU power (TGP) override: -1 = Auto (mode table), 0..2 = Min/Med/Max.
  int GetGpuPowerOverride() { return m_gpuPowerOverride; }
  void SetGpuPowerOverride(int level) { m_gpuPowerOverride = level; }
  int GetEffectiveGpuPowerLevel() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_gpuPowerOverride >= 0 && m_gpuPowerOverride <= 2)
      return m_gpuPowerOverride;
    switch (m_currentMode) {
    case PowerMode::Eco: return 0;
    case PowerMode::Balanced: return 1;
    case PowerMode::Performance:
    default: return 2;
    }
  }
  void SetAcEnabled(bool v) {
    m_acEnabled = v;
    m_acLastLine = -1; // re-baseline on toggle
  }

  // Wake-on-LAN (Wired NIC Magic Packet + HP BIOS S3/S4/S5 Wake-on-LAN)
  bool GetWakeOnLan();
  bool SetWakeOnLan(bool enable);

  // Wake-on-WLAN & Bluetooth (Wireless M.2 Magic Packet + HP BIOS Wake on WLAN/BT)
  bool GetWakeOnWlanBt();
  bool SetWakeOnWlanBt(bool enable);

  // Low-Latency Network & OS Gaming Tweak (NetworkThrottlingIndex = 0xFFFFFFFF, SystemResponsiveness = 0)
  bool GetNetworkGamingTweak();
  bool SetNetworkGamingTweak(bool enable);

  // AC-line auto-switch: on battery → Eco+Quiet (saving current state),
  // on AC → restore. Call periodically from the worker loop.
  void CheckAcLine();

  // GPU power (TGP): 0=Min, 1=Med, 2=Max (WMI 0x22). Public: also driven by
  // the UI pills and API override.
  bool SetGpuPower(uint8_t level);

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
  bool SetFanMax(bool enabled); // WMI Method 0x27 (Max Fan Trigger)

private:
  PowerControl();

  std::mutex m_mutex;
  PowerMode m_currentMode = PowerMode::Balanced;
  bool m_maxFanActive = false;
  int m_gpuMode = -1; // 0=Hybrid, 1=Discrete, 2=Optimus
  int m_batteryLimitPercent = 100; // Cached battery threshold (WMI only knows on/off)
  bool m_displayOverdrive = true;  // Cached display overdrive
  int m_amdCurveOptimizer = 0;
  int m_gpuPowerOverride = -1; // -1=Auto, 0..2=Min/Med/Max TGP override

  // AC-line auto-switch state
  bool m_acEnabled = false;   // feature toggle (config)
  int m_acLastLine = -1;      // last known ACLineStatus (-1 unknown)
  bool m_acSaved = false;
  PowerMode m_acSavedMode = PowerMode::Balanced;
  int m_acSavedProfile = 0;

  // Wake cached state (refreshed at startup + after toggle)
  int m_wolCached = -1; // -1 unknown, 0 off, 1 on
  int m_wlanBtWolCached = -1; // -1 unknown, 0 off, 1 on
  int m_netGamingCached = -1; // -1 unknown, 0 off, 1 on

  // WMI HP BIOS Helper
  bool CallHpBios(uint32_t cmd, uint32_t type, uint8_t *data, size_t size,
                  size_t expectedOutSize = 0, WmiHelper *pWmi = nullptr);

  // Windows Power Helper
  void SetWindowsPowerPlan(PowerMode mode);

  // Internal helpers
  void CheckThermalPolicy();

  enum class ThermalPolicyVersion { V0_Legacy, V1_Modern, Unknown };
  ThermalPolicyVersion m_thermalPolicy = ThermalPolicyVersion::Unknown;

  // Persistent WMI helper for background thread
  WmiHelper m_wmiBg;
};
