#pragma once
#include "PowerControl.h"
#include "SmartHelper.h" // For DriveInfo struct
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


class OmenHal {
public:
  static OmenHal &Get();

  bool Initialize();
  bool IsInitialized() const { return m_initialized; }
  void Shutdown();
  void Update();


  // Getters (delegates to services)
  float GetCpuTemp();
  float GetGpuTemp();
  float GetCpuLoad();
  float GetGpuLoad();
  float GetTotalPower();
  const std::vector<DriveInfo> &GetDriveTemps();
  int GetEcErrorCount() { return 0; }

  float GetFanSpeed(int fanIndex);
  float GetFanPercentage(int fanIndex);
  bool GetDriverStatus();
  bool GetFanManual();

  std::string GetCpuName() { return m_cpuName; }
  std::string GetGpuName() { return m_gpuName; }

  // Setters
  void SetFanSpeed(int fanIndex, int rpm);
  void SetFanAuto();

  // Fan Curve & Modes
  int GetFanControlMode(); // 0=Auto, 1=Manual, 2=Sync, 3=Optimized, 4=Separated
  void SetFanControlMode(int mode);
  void *GetCpuCurve();
  void *GetGpuCurve();

  PowerControl::GpuOverclockSettings GetGpuOverclockSettings();
  bool SetGpuOverclock(const PowerControl::GpuOverclockSettings &settings);

  // Battery Care
  int GetBatteryChargeLimit();
  bool SetBatteryChargeLimit(int percentage);

  // CPU Undervolting
  int GetCpuCoreOffset();
  int GetCpuCacheOffset();
  bool SetCpuUndervolt(int coreMv, int cacheMv);
  int GetAmdCurveOptimizer();
  bool SetAmdCurveOptimizer(int coCounts);
  int GetCachedAmdCurveOptimizer();
  void SetCachedAmdCurveOptimizer(int val);

  bool IsAmd() { return m_cpuBrand == 1; }
  bool IsIntel() { return m_cpuBrand == 2; }


  // Desktop
  bool GetIsDesktop();
  bool GetIsAnotherFanControllerActive();

  // Power/Performance
  void SetPowerMode(int mode); // 0=Eco, 1=Balanced, 2=Performance
  int GetPowerMode();
  std::string GetGpuModeStr();
  int GetGpuModeInt();
  void RequestGpuMode(int mode);
  void OptimizeMemory();

private:
  OmenHal();
  ~OmenHal();

  void BackgroundLoop();

  std::thread m_workerThread;
  bool m_stopWorker = false;
  bool m_initialized = false;

  std::string m_cpuName = "Unknown CPU";
  std::string m_gpuName = "Unknown GPU";
  int m_cpuBrand = 0; // 0=Unknown, 1=AMD, 2=Intel

  bool m_isDesktop = false;
  bool m_anotherFanControllerActive = false;
};
