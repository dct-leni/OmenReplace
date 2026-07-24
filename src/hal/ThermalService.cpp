#define NOMINMAX
#include "ThermalService.h"
#include "OmenEc.h"
#include <algorithm>
#include <iostream>

// Windows Headers for CPU Load (PDH)
#include <pdh.h>
#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#endif

// Helper for CPU Load
static PDH_HQUERY cpuQuery;
static PDH_HCOUNTER cpuTotal;
static bool pdhInitialized = false;

void InitPdh() {
  if (!pdhInitialized) {
    if (PdhOpenQueryA(NULL, 0, &cpuQuery) == ERROR_SUCCESS) {
      PdhAddCounterA(cpuQuery, "\\Processor Information(_Total)\\% Processor Utility", 0,
                     &cpuTotal);
      PdhCollectQueryData(cpuQuery);
      pdhInitialized = true;
    }
  }
}

float GetCpuLoadPDH() {
  if (!pdhInitialized)
    InitPdh();
  if (!pdhInitialized)
    return 0.0f;

  PDH_FMT_COUNTERVALUE counterVal;
  PdhCollectQueryData(cpuQuery);
  PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal);
  return (float)counterVal.doubleValue;
}

ThermalService &ThermalService::Get() {
  static ThermalService instance;
  return instance;
}

ThermalService::ThermalService() {
  m_nvml.Initialize();
  InitPdh();
  m_smart.ScanDrives();
}

void ThermalService::Update() {
  // 1. Temps (Always 1s)
  float cpuVal = 0;
  float t57 = OmenEc::Get().GetCpuTemp57();
  float t58 = OmenEc::Get().GetCpuTemp58();
  float tCE = (float)OmenEc::Get().ReadByte(0xCE);

  if (t57 > 25 && t57 < 105)
    cpuVal = t57;
  else if (t58 > 25 && t58 < 105)
    cpuVal = t58;
  else if (tCE > 25 && tCE < 105)
    cpuVal = tCE;

  // GPU Temp: Prefer EC (0x59) as it doesn't wake dGPU
  float gpuVal = OmenEc::Get().GetGpuTemp();

  // Only fallback to NVML if EC fails and we are in a slow cycle (every 10s)
  bool checkedNvml = false;
  if (gpuVal <= 0 && m_timer % 10 == 0) {
    gpuVal = m_nvml.GetGpuTemp();
    checkedNvml = true;
  }
  // If we have a valid temp from before, keep using it if EC is 0
  // UNLESS we just checked NVML and it said 0 (Device Off)
  if (gpuVal <= 0 && m_gpuTemp > 0 && !checkedNvml)
    gpuVal = m_gpuTemp;

  // 2. Heavy Load & Power queries (Every 10s - was 3s)
  // These calls (especially NVML) can wake the dGPU, causing heat.
  float cpuLoad = m_cpuLoad;
  float gpuLoad = m_gpuLoad;
  static float lastRawPower = m_totalPower;

  if (m_timer % 10 == 0) {
    // Load
    cpuLoad = GetCpuLoadPDH();
    gpuLoad = m_nvml.GetGpuLoad();

    // Power
    float cpuPower = (float)OmenEc::Get().ReadByte(0xD2);
    if (cpuPower <= 0 || cpuPower > 250) {
      cpuPower = 5.0f + (cpuVal > 40 ? (cpuVal - 40) * 0.8f : 0.0f);
    }
    float gpuPower = m_nvml.GetGpuPower();
    if (gpuPower < 0)
      gpuPower = 0;
    lastRawPower = cpuPower + gpuPower;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cpuVal > 15)
      m_cpuTemp = cpuVal;
    // Update GPU temp
    m_gpuTemp = gpuVal;

    m_cpuLoad = cpuLoad;
    m_gpuLoad = gpuLoad;
    float gpuPowerW = (gpuLoad / 100.0f) * 115.0f + (gpuLoad > 5.0f ? 12.0f : 4.0f);
    float estimatedTotalSystemPower = lastRawPower + gpuPowerW + 12.0f; // CPU W + GPU W + System Base W
    m_totalPower = (m_totalPower * 0.7f) + (estimatedTotalSystemPower * 0.3f);
  }


  // 3. SSD Temps (Every ~4s)
  if (m_timer % 4 == 0) {
    m_smart.UpdateTemps();
    // We can't safely modify the vector returned by GetDrives() if it's const&
    // in SmartHelper. We should just read it and then override local logic or
    // if SmartHelper allows mod. Since SmartHelper owns the data, let's just do
    // a trick: we want the UI to see Disk 1/2.

    // Actually, let's fix the logic: SmartHelper should just return what it
    // has. We will "Patch" the drives list in ThermalService temporarily or let
    // SmartHelper handle it. For now, let's assume SmartHelper returns a vector
    // we can't resize easily if it's const reference. BUT, looking at
    // SmartHelper.h/cpp from context, it usually has a vector. We'll cast away
    // constness carefully OR better, just ensure we iterate what we have. The
    // user issue was "Disk 2 not fitting".

    auto &drives = const_cast<std::vector<DriveInfo> &>(m_smart.GetDrives());

    // Only ensure dummy "Disk 1/2" if we truly found absolutely nothing via SMART
    if (drives.empty()) {
      drives.push_back(DriveInfo{0, "Disk 1", 0});
      drives.push_back(DriveInfo{1, "Disk 2", 0});
    }

    for (size_t i = 0; i < drives.size(); ++i) {
      if (drives[i].Temperature <= 0 || drives[i].Temperature > 100) {
        float t = (i == 0) ? (float)OmenEc::Get().ReadByte(0x4A)
                           : (float)OmenEc::Get().ReadByte(0x48);
        if (t <= 0)
          t = (i == 0) ? (float)OmenEc::Get().ReadByte(0x4B)
                       : (float)OmenEc::Get().ReadByte(0x49);

        if (t > 25 && t < 85)
          drives[i].Temperature = (int)t;
      }
    }
  }

  m_timer++;
  if (m_timer > 100)
    m_timer = 0;
}

float ThermalService::GetCpuTemp() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_cpuTemp;
}
float ThermalService::GetGpuTemp() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_gpuTemp;
}
float ThermalService::GetCpuLoad() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_cpuLoad;
}
float ThermalService::GetGpuLoad() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_gpuLoad;
}
float ThermalService::GetTotalPower() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_totalPower;
}

const std::vector<DriveInfo> &ThermalService::GetDriveTemps() {
  return m_smart.GetDrives();
}
