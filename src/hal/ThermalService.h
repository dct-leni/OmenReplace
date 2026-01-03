#pragma once
#include "NvmlHelper.h"
#include "SmartHelper.h"
#include <mutex>
#include <string>
#include <vector>


class ThermalService {
public:
  static ThermalService &Get();

  void Update(); // Called by HAL loop

  float GetCpuTemp();
  float GetGpuTemp();
  float GetCpuLoad();
  float GetGpuLoad();
  float GetTotalPower();
  const std::vector<DriveInfo> &GetDriveTemps();

private:
  ThermalService();

  std::mutex m_mutex;
  float m_cpuTemp = 0.0f;
  float m_gpuTemp = 0.0f;
  float m_cpuLoad = 0.0f;
  float m_gpuLoad = 0.0f;
  float m_totalPower = 0.0f;

  NvmlHelper m_nvml;
  SmartHelper m_smart;

  int m_timer = 0;
};
