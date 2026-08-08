#pragma once
#include "NvmlHelper.h"
#include "SmartHelper.h"
#include "WmiHelper.h"
#include <mutex>
#include <string>
#include <vector>


class ThermalService {
public:
  static ThermalService &Get();

  void Update(); // Called by HAL loop

  float GetCpuTemp();
  float GetGpuTemp();
  float GetRamTemp(); // DIMM thermal via PIIX4 SMBus; 0 if unavailable
  // Per-DIMM temps (2 slots; a slot is 0 if that DIMM has no sensor). Returns
  // the number of DIMMs with a live sensor (0..2).
  int GetRamTemps(float &t0, float &t1);
  float GetCpuLoad();
  float GetGpuLoad();
  float GetTotalPower();
  float GetCpuPower();
  float GetGpuPower();
  const std::vector<DriveInfo> &GetDriveTemps();

private:
  ThermalService();

  std::mutex m_mutex;
  float m_cpuTemp = 0.0f;
  float m_gpuTemp = 0.0f;
  float m_ramTemp = 0.0f;
  float m_ramTemp0 = 0.0f; // DIMM 0 temp
  float m_ramTemp1 = 0.0f; // DIMM 1 temp
  bool m_smbusReady = false;
  float m_cpuLoad = 0.0f;
  float m_gpuLoad = 0.0f;
  float m_totalPower = 0.0f;
  float m_cpuPower = 0.0f;
  float m_gpuPower = 0.0f;

  NvmlHelper m_nvml;
  SmartHelper m_smart;
  WmiHelper m_wmiTemp; // persistent WMI for CPU temp fallback

  int m_timer = 0;
};
