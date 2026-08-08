#include "ThermalService.h"
#include "OmenEc.h"
#include "OmenLog.h"
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
  m_smart.ScanDrives();
}

void ThermalService::Update() {
  // PDH CPU load must be initialized on the worker thread.
  InitPdh();

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

  // SMU Tctl fallback (EC 0x57/0x58 unpopulated on some models). Ryzen
  // THM_TCON_CUR_TMP at SMN 0x59800: Tctl in bits [22:16] (Celsius).
  if (cpuVal <= 0) {
    for (int attempt = 0; attempt < 3 && cpuVal <= 0; attempt++) {
      uint32_t thm = 0;
      if (OmenEc::Get().SmuReadReg(0x59800, thm)) {
        int tctl = (int)((thm >> 16) & 0xFF);
        if (tctl > 25 && tctl < 105) {
          cpuVal = (float)tctl;
          break;
        }
      }
    }
  }

  // WMI BIOS CPU temp fallback. CMD 0x23 {0x01,0,0,0} returns result[0].
  if (cpuVal <= 0) {
    if (m_wmiTemp.Initialize()) {
      uint8_t d[4] = {0x01, 0x00, 0x00, 0x00};
      std::vector<uint8_t> out;
      if (m_wmiTemp.ExecuteHpBiosMethod(0x20008, 0x23, d, 4, out, 4)) {
        if (!out.empty() && out[0] > 25 && out[0] < 105)
          cpuVal = (float)out[0];
      }
    }
  }

  // GPU Temp: Prefer NVML (accurate) since EC 0x59 may be stale on this model.
  float gpuVal = m_nvml.GetGpuTemp();
  bool checkedNvml = true;
  if (gpuVal <= 0) {
    // NVML unavailable or GPU asleep — fall back to EC
    gpuVal = OmenEc::Get().GetGpuTemp();
    checkedNvml = false;
  }
  // If we have a valid temp from before, keep using it if EC is 0
  // UNLESS we just checked NVML and it said 0 (Device Off)
  if (gpuVal <= 0 && m_gpuTemp > 0 && !checkedNvml)
    gpuVal = m_gpuTemp;

  // 2. Heavy Load & Power queries (Every 2s)
  float cpuLoad = m_cpuLoad;
  float gpuLoad = m_gpuLoad;
  static float lastRawPower = m_totalPower;

  if (m_timer % 2 == 0) {
    // Load
    cpuLoad = GetCpuLoadPDH();
    gpuLoad = m_nvml.GetGpuLoad();

    // RAM temp via SMBus PIIX4 module (EnableSmbusPci once, then read both
    // DIMM slots each cycle — the SPD responds immediately, no timeout stall).
    if (!m_smbusReady)
      m_smbusReady = OmenEc::Get().EnableSmbusPci();
    if (m_smbusReady) {
      float t0 = OmenEc::Get().GetDimmTemp(0);
      float t1 = OmenEc::Get().GetDimmTemp(1);
      std::lock_guard<std::mutex> lock(m_mutex);
      if (t0 > 0 && t0 < 100) m_ramTemp0 = t0;
      if (t1 > 0 && t1 < 100) m_ramTemp1 = t1;
      // m_ramTemp = highest valid (fallback for single-value consumers).
      m_ramTemp = std::max(m_ramTemp0, m_ramTemp1);
    }

    // Power
    float cpuPower = OmenEc::Get().GetCpuPackagePower();
    if (cpuPower <= 0) {
      // MSR RAPL unavailable — fall back to EC estimate
      float ec = (float)OmenEc::Get().ReadByte(0xD2);
      cpuPower = ec > 0 ? std::min(ec * 2.0f, 150.0f)
                        : (5.0f + (cpuVal > 40 ? (cpuVal - 40) * 0.8f : 0.0f));
    }
    float gpuPower = m_nvml.GetGpuPower();
    if (gpuPower <= 0) {
      // NVML unavailable or GPU asleep — estimate from load
      gpuPower = (gpuLoad / 100.0f) * 115.0f + (gpuLoad > 5.0f ? 12.0f : 4.0f);
    }
    lastRawPower = cpuPower + gpuPower + 12.0f;
    OmenLog("[OMEN] power_debug cpu_raw=%d gpu_raw=%d total_est=%d\n",
            (int)cpuPower, (int)gpuPower, (int)lastRawPower);

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_cpuPower = cpuPower;
      m_gpuPower = gpuPower;
    }
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cpuVal > 15)
      m_cpuTemp = cpuVal;
    // Update GPU temp
    m_gpuTemp = gpuVal;

    m_cpuLoad = cpuLoad;
    m_gpuLoad = gpuLoad;
    m_totalPower = (m_totalPower * 0.7f) + (lastRawPower * 0.3f);
  }


  // 3. SSD Temps (Every ~4s)
  if (m_timer % 4 == 0) {
    m_smart.UpdateTemps();
    // EC fallback for disks where SMART reports no temp (this model).
    auto &drives = m_smart.GetDrives();
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
float ThermalService::GetRamTemp() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_ramTemp;
}
int ThermalService::GetRamTemps(float &t0, float &t1) {
  std::lock_guard<std::mutex> lock(m_mutex);
  t0 = m_ramTemp0;
  t1 = m_ramTemp1;
  int n = 0;
  if (t0 > 0 && t0 < 100) n++;
  if (t1 > 0 && t1 < 100) n++;
  return n;
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
float ThermalService::GetCpuPower() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_cpuPower;
}
float ThermalService::GetGpuPower() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_gpuPower;
}

const std::vector<DriveInfo> &ThermalService::GetDriveTemps() {
  return m_smart.GetDrives();
}
