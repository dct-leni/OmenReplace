#pragma once
#include "PowerControl.h"
#include "SmartHelper.h" // For DriveInfo struct
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class OmenHal {
public:
  static OmenHal &Get();

  bool Initialize();
  bool IsInitialized() const { return m_initialized; }
  bool IsFanControlReady() const { return m_fanControlReady.load(); }
  bool IsFanControlActive() const { return m_fanControlActive.load(); }
  void SetFanControlActive(bool v) { m_fanControlActive = v; }
  void Shutdown();

  float GetCpuTemp();
  float GetGpuTemp();
  float GetRamTemp();
  int GetRamTemps(float &t0, float &t1);
  float GetCpuLoad();
  float GetGpuLoad();
  float GetTotalPower();
  float GetCpuPower();
  float GetGpuPower();
  const std::vector<DriveInfo> &GetDriveTemps();

  float GetFanSpeed(int fanIndex);
  float GetFanPercentage(int fanIndex);
  bool GetDriverStatus();
  void SetFanAuto();

  const std::string &GetCpuName() const { return m_cpuName; }
  const std::string &GetGpuName() const { return m_gpuName; }

  // Fan Curve & Modes
  int GetFanControlMode();
  void SetFanControlMode(int mode);

  // Battery Care
  int GetBatteryChargeLimit();
  bool SetBatteryChargeLimit(int percentage);

  // Display Panel Overdrive
  bool GetDisplayOverdrive();
  bool SetDisplayOverdrive(bool enable);

  // Wake on LAN (Ethernet + BIOS S3/S4/S5) & Wake on WLAN/BT
  bool GetWakeOnLan();
  bool SetWakeOnLan(bool enable);
  bool GetWakeOnWlanBt();
  bool SetWakeOnWlanBt(bool enable);

  int GetAmdCurveOptimizer();
  bool SetAmdCurveOptimizer(int coCounts);
  int GetCachedAmdCurveOptimizer();
  // CPU power limits via MP1 SMU (STAPM 0x4F). watts 15..54, 0 = skip.
  bool SetStapmLimit(int watts);
  // Read current power/temp limits. Returns false if unavailable.
  bool GetPowerThermalLimits(int &powerW, int &tempC);
  // Set CPU temperature (Tctl) limit via MP1 0x3F. 75..105°C.
  bool SetTctlTemp(int tempC);

  // Desktop
  bool GetIsDesktop();
  bool GetIsAnotherFanControllerActive();

  // Power/Performance
  void SetPowerMode(int mode); // 0=Eco, 1=Balanced, 2=Performance, 3=Turbo
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
  std::condition_variable m_workerWake;
  std::mutex m_workerWakeMutex;
  std::atomic<bool> m_stopWorker{false};
  std::atomic<bool> m_initialized{false};
  std::atomic<bool> m_fanControlReady{false};
  std::atomic<bool> m_fanControlActive{false};

  bool m_isDesktop = false;
  bool m_anotherFanControllerActive = false;

  std::string m_cpuName;
  std::string m_gpuName;
};
