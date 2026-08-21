#include "PowerControl.h"
#include "FanService.h"
#include "OmenEc.h"
#include "OmenLog.h"
#include "WmiHelper.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <powrprof.h>
#include <thread>

#ifdef _MSC_VER
#pragma comment(lib, "powrprof.lib")
#endif

// Windows Power Plan GUIDs (Standard)
static const GUID GUID_SAVER = {
    0xa1841308,
    0x3541,
    0x4fab,
    {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
static const GUID GUID_BALANCED = {
    0x381b4222,
    0xf694,
    0x41f0,
    {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
static const GUID GUID_PERF = {
    0x8c5e7fda,
    0xe8bf,
    0x4a96,
    {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};

// Windows 10/11 Overlay Schemes (Hidden)
static const GUID GUID_OVERLAY_EFFICIENCY = {
    0x961cc777,
    0x2547,
    0x4f9d,
    {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}}; // Better Battery
static const GUID GUID_OVERLAY_PERFORMANCE = {
    0xded574b5,
    0x45a0,
    0x4f42,
    {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}}; // Best Performance
static const GUID GUID_OVERLAY_BALANCED = {
    0x00000000,
    0x0000,
    0x0000,
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}}; // Balanced (None)

// Windows Processor Subgroup & Settings GUIDs
static const GUID GUID_PROCESSOR_SETTINGS_SUBGROUP = {
    0x54533251,
    0x82be,
    0x4824,
    {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
static const GUID GUID_PERFBOOST = {
    0xbe337238,
    0x0d82,
    0x4146,
    {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
static const GUID GUID_PERFEPP = {
    0x36687f9e,
    0xe3a5,
    0x4dbf,
    {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}};

// Function Pointers for Hidden APIs
typedef DWORD(WINAPI *PfnPowerSetActiveOverlayScheme)(GUID *);
typedef DWORD(WINAPI *PfnPowerGetActualOverlayScheme)(GUID *);

PowerControl &PowerControl::Get() {
  static PowerControl instance;
  return instance;
}

// BackgroundLoop owns persistent WMI access. Constructing here can bind the
// helper to the UI thread before the worker starts.
PowerControl::PowerControl() {}

void PowerControl::Update() {
  // 1. Read current mode from EC
  uint8_t mode = OmenEc::Get().ReadByte(0xCE);

  PowerMode detectedEc = m_currentMode;
  // Support both Legacy (0,1,2) and V1 (0x30, 0x31, 0x50)
  if (mode == 0x00 || mode == 0x30)
    detectedEc = PowerMode::Balanced;
  else if (mode == 0x01 || mode == 0x31)
    detectedEc = PowerMode::Performance;
  else if (mode == 0x02 || mode == 0x50 || mode == 0x03)
    detectedEc = PowerMode::Eco;

  // 2. Read Windows Power Plan (free active plan GUID to avoid leak)
  GUID *activePlan = NULL;
  if (PowerGetActiveScheme(NULL, &activePlan) == ERROR_SUCCESS) {
    LocalFree(activePlan);
  }

  // 3. Update Windows Overlay Scheme
  PowerMode detectedOverlay = detectedEc; // Fallback to EC
  HMODULE hPowr = GetModuleHandleA("powrprof.dll");
  if (!hPowr)
    hPowr = LoadLibraryA("powrprof.dll");
  if (hPowr) {
    auto pGetActualOverlay = (PfnPowerGetActualOverlayScheme)GetProcAddress(
        hPowr, "PowerGetActualOverlayScheme");
    if (pGetActualOverlay) {
      GUID overlay;
      if (pGetActualOverlay(&overlay) == ERROR_SUCCESS) {
        if (IsEqualGUID(overlay, GUID_OVERLAY_EFFICIENCY))
          detectedOverlay = PowerMode::Eco;
        else if (IsEqualGUID(overlay, GUID_OVERLAY_PERFORMANCE))
          detectedOverlay = PowerMode::Performance;
        else if (IsEqualGUID(overlay, GUID_OVERLAY_BALANCED))
          detectedOverlay = PowerMode::Balanced;
      }
    }
  }

  // Synchronize
  std::lock_guard<std::mutex> lock(m_mutex);
  // Trust Overlay change if it differs from current (User moved slider)
  if (detectedOverlay != m_currentMode) {
    m_currentMode = detectedOverlay;
  }
  // Otherwise trust EC hardware change
  else if (detectedEc != m_currentMode) {
    m_currentMode = detectedEc;
  }
}

void PowerControl::InitGpuMux() {
  WmiHelper wmi;
  if (wmi.Initialize()) {
    std::vector<uint8_t> out;
    if (wmi.ExecuteHpBiosMethod(0x00001, 0x52, NULL, 0, out, 4)) {
      if (!out.empty() && out[0] <= 2) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_gpuMode = out[0];
      }
    }
  }
}

void PowerControl::CheckThermalPolicy() {
  if (m_thermalPolicy != ThermalPolicyVersion::Unknown)
    return;

  WmiHelper wmi;
  if (!wmi.Initialize())
    return;

  std::vector<uint8_t> out;
  // CMD_SYSTEM_GET_DATA = 0x28, expect 128 bytes
  if (wmi.ExecuteHpBiosMethod(0x20008, 0x28, NULL, 0, out, 128)) {
    if (out.size() >= 4) {
      uint8_t version = out[3];
      m_thermalPolicy = (version == 0) ? ThermalPolicyVersion::V0_Legacy
                                       : ThermalPolicyVersion::V1_Modern;
    }
  }
}

bool PowerControl::SetGpuPower(uint8_t level) {
  // level: 0=Min (no TGP boost, no PPAB), 1=Med (TGP enabled, no PPAB), 2=Max (TGP + PPAB enabled)
  uint8_t data[4] = {0};
  switch (level) {
  case 0: // Min
    data[0] = 0;
    data[1] = 0;
    break;
  case 1: // Med
    data[0] = 1;
    data[1] = 0;
    break;
  case 2: // Max
    data[0] = 1;
    data[1] = 1;
    break;
  default:
    data[0] = 1;
    data[1] = 0;
    break;
  }
  data[2] = 0x01; // DState = D1
  data[3] = 0x00; // PeakTemp

  return CallHpBios(0x20008, 0x22, data, 4, 0);
}

bool PowerControl::SetFanLevelWmiBg(int cpuPercent, int gpuPercent) {
  // CMD_FAN_SET_LEVEL = 0x2E
  // Scaling: 55 = ~5500 RPM (100%)
  uint8_t cpuLevel = (uint8_t)(cpuPercent * 55 / 100);
  uint8_t gpuLevel = (uint8_t)(gpuPercent * 55 / 100);
  uint8_t data[4] = {cpuLevel, gpuLevel, 0, 0};

  // Use persistent background WMI helper
  return CallHpBios(0x20008, 0x2E, data, 4, 0, &m_wmiBg);
}

bool PowerControl::SetFanLevelWmi(int cpuPercent, int gpuPercent) {
  // CMD_FAN_SET_LEVEL = 0x2E
  // Scaling: 55 = ~5500 RPM (100%)
  uint8_t cpuLevel = (uint8_t)(cpuPercent * 55 / 100);
  uint8_t gpuLevel = (uint8_t)(gpuPercent * 55 / 100);
  uint8_t data[4] = {cpuLevel, gpuLevel, 0, 0};

  bool success = CallHpBios(0x20008, 0x2E, data, 4, 0);

  return success;
}

void PowerControl::RequestGpuMode(int mode) {
  if (GetGpuModeInt() == mode)
    return;
  // Usually WMI CMD 0x20008, type 0x3E, data { mode }
  uint8_t d[4] = {(uint8_t)mode};
  CallHpBios(0x20008, 0x3E, d, 4, 0);
  m_gpuMode = mode;
}

int PowerControl::GetBatteryChargeLimit() {
  // Primary: persisted config (survives restarts). Saved by SetBatteryChargeLimit.
  auto &oc = FanService::Get().GetOverlayConfig();
  int cfgLimit = oc.batteryLimit;
  if (cfgLimit >= 60 && cfgLimit <= 100) {
    m_batteryLimitPercent = cfgLimit;
    return cfgLimit;
  }
  // Fallback: WMI 0x24 readback. May fail early in startup (WMI not ready).
  WmiHelper wmi;
  if (!wmi.Initialize())
    return m_batteryLimitPercent;

  uint8_t data[4] = {0};
  std::vector<uint8_t> out;
  if (wmi.ExecuteHpBiosMethod(0x20008, 0x24, data, 4, out, 4) &&
      out.size() >= 1) {
    int readLimit = (out[0] == 0x01) ? 80 : 100;
    m_batteryLimitPercent = readLimit;
    oc.batteryLimit = readLimit;
    return readLimit;
  }

  return m_batteryLimitPercent; // WMI failed — last known value
}

bool PowerControl::SetBatteryChargeLimit(int limitPercent) {
  // HP WMI BIOS Method 0x24 expects: 0x01 = Enabled (~80% limit), 0x00 = Disabled (100% full charge)
  int normalizedLimit = (limitPercent <= 80) ? 80 : 100;
  uint8_t data[4] = {0};
  data[0] = (normalizedLimit == 80) ? 0x01 : 0x00;

  m_batteryLimitPercent = normalizedLimit;

  auto &oc = FanService::Get().GetOverlayConfig();
  oc.batteryLimit = normalizedLimit;
  FanService::Get().SaveConfig();

  WmiHelper wmi;
  if (!wmi.Initialize())
    return false;
  std::vector<uint8_t> out;
  return wmi.ExecuteHpBiosMethod(0x20008, 0x24, data, 4, out, 0);
}

void PowerControl::RestoreFanAuto() {
  // 1. Force BIOS to re-read thermal policy by switching away and back (The
  // "Mode Jiggle") This often breaks the manual lock on 2023+ Omen models
  uint8_t cool[4] = {0xFF, 0x50, 0x00, 0x00}; // Cool/Quiet Mode
  CallHpBios(0x20008, 0x1A, cool, 4, 0);
  Sleep(100);

  uint8_t def[4] = {0xFF, 0x30, 0x00, 0x00}; // Default/Balanced Mode
  CallHpBios(0x20008, 0x1A, def, 4, 0);

  // 2. Disable Max Fan via BIOS method 0x27
  uint8_t maxData[4] = {0x00, 0x00, 0x00, 0x00};
  CallHpBios(0x20008, 0x27, maxData, 4, 0);

  // 3. Reset the idle watchdog/manual extension (0x31)
  uint8_t idleData[4] = {0x00, 0x00, 0x00, 0x00};
  CallHpBios(0x20008, 0x31, idleData, 4, 0);
}

bool PowerControl::SetCpuPowerLimit(int pl1Watts, int pl2Watts) {
  // WMI Method 0x29: { pl2, pl1, pl4, tpp }
  // Sentinel 0xFF means keep current / unchanged.
  // Clamp to 254 max because 255 (0xFF) is the EC "keep unchanged" sentinel.
  uint8_t pl1 = (uint8_t)std::max(10, std::min(254, pl1Watts));
  uint8_t pl2 = (uint8_t)std::max(10, std::min(254, pl2Watts));
  uint8_t data[4] = { pl2, pl1, 0xFF, 0xFF };
  return CallHpBios(0x20008, 0x29, data, 4, 0);
}

bool PowerControl::SetThermalPolicy(uint8_t modeByte) {
  // WMI Method 0x1A: { 0xFF, modeByte, 0x00, 0x00 }
  // 0x30 = Default/Balanced (PerformanceMode.L2), 0x31 = Performance/Unleash (PerformanceMode.L7)
  uint8_t data[4] = { 0xFF, modeByte, 0x00, 0x00 };
  return CallHpBios(0x20008, 0x1A, data, 4, 0);
}

bool PowerControl::SetAmdAllPptLimits(int fastW, int slowW, int stapmW) {
  OmenEc &ec = OmenEc::Get();
  if (!ec.IsInitialized()) return false;

  bool anyOk = false;

  // 1. Fast PPT (Burst limit in mW)
  uint32_t argsFast[6] = { (uint32_t)(fastW * 1000), 0, 0, 0, 0, 0 };
  if (ec.SendMp1Command(0x3E, argsFast)) anyOk = true;
  else {
    uint32_t rFast[6] = { (uint32_t)(fastW * 1000), 0, 0, 0, 0, 0 };
    if (ec.SendSmuCommand(0x56, rFast)) anyOk = true;
  }

  // 2. Slow PPT (Sustained boost limit in mW)
  uint32_t argsSlow[6] = { (uint32_t)(slowW * 1000), 0, 0, 0, 0, 0 };
  if (ec.SendMp1Command(0x5F, argsSlow)) anyOk = true;
  else {
    uint32_t rSlow[6] = { (uint32_t)(slowW * 1000), 0, 0, 0, 0, 0 };
    if (ec.SendSmuCommand(0xCB, rSlow)) anyOk = true;
  }

  // 3. STAPM (Sustained envelope limit in mW)
  uint32_t argsStapm[6] = { (uint32_t)(stapmW * 1000), 0, 0, 0, 0, 0 };
  if (ec.SendMp1Command(0x4F, argsStapm)) anyOk = true;

  return anyOk;
}

void PowerControl::SetCpuBoostMode(int boostMode) {
  // Boost modes: 0=Disabled, 1=Enabled, 2=Aggressive, 3=Efficient Enabled, 4=Efficient Aggressive
  GUID *activeScheme = nullptr;
  if (PowerGetActiveScheme(NULL, &activeScheme) == ERROR_SUCCESS && activeScheme) {
    PowerWriteACValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFBOOST, (DWORD)boostMode);
    PowerWriteDCValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFBOOST, (DWORD)boostMode);
    PowerSetActiveScheme(NULL, activeScheme);
    LocalFree(activeScheme);
  }
}

static void SetEppPreference(DWORD eppVal) {
  // EPP: 0=Max Performance, 50=Balanced, 80=Energy Efficient
  GUID *activeScheme = nullptr;
  if (PowerGetActiveScheme(NULL, &activeScheme) == ERROR_SUCCESS && activeScheme) {
    PowerWriteACValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFEPP, eppVal);
    PowerWriteDCValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFEPP, eppVal);
    PowerSetActiveScheme(NULL, activeScheme);
    LocalFree(activeScheme);
  }
}

void PowerControl::SetMode(PowerMode mode) {
  CheckThermalPolicy();

  // 1. Set HP WMI Thermal Policy (0x1A) and EC Mode Byte (0xCE)
  uint8_t thermalByte = (mode == PowerMode::Performance || mode == PowerMode::Turbo) ? 0x31 : 0x30;
  SetThermalPolicy(thermalByte);

  uint8_t ecValue = 0x00;
  switch (mode) {
  case PowerMode::Eco:
    ecValue = 0x02;
    break;
  case PowerMode::Balanced:
    ecValue = 0x00;
    break;
  case PowerMode::Performance:
  case PowerMode::Turbo:
    ecValue = 0x01;
    break;
  }
  OmenEc::Get().WriteByte(0xCE, ecValue);

  // 2. Yield briefly so the EC finishes its internal mode transition
  // without wiping our upcoming custom power limit configuration.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 3. Configure CPU (WMI 0x29) and GPU (WMI 0x22) limits per mode
  int pl1 = 45, pl2 = 54;
  uint8_t gpuLvl = 1;
  int fastPpt = 54, slowPpt = 45, stapmPpt = 45;

  switch (mode) {
  case PowerMode::Eco:
    pl1 = 25; pl2 = 35;
    gpuLvl = 0;
    fastPpt = 35; slowPpt = 25; stapmPpt = 25;
    break;
  case PowerMode::Balanced:
    pl1 = 45; pl2 = 54;
    gpuLvl = 1;
    fastPpt = 54; slowPpt = 45; stapmPpt = 45;
    break;
  case PowerMode::Performance:
    pl1 = 75; pl2 = 90;
    gpuLvl = 2;
    fastPpt = 90; slowPpt = 75; stapmPpt = 75;
    break;
  case PowerMode::Turbo:
    pl1 = 254; pl2 = 254; // Uncapped
    gpuLvl = 2;
    fastPpt = 120; slowPpt = 95; stapmPpt = 85;
    break;
  }

  SetCpuPowerLimit(pl1, pl2);
  SetGpuPower(gpuLvl);

  // 4. Send AMD SMU PPT limits
  SetAmdAllPptLimits(fastPpt, slowPpt, stapmPpt);

  // 5. Windows Power Plan & CPU Boost
  SetWindowsPowerPlan(mode);

  OmenLog("[AMDOMEN] SetMode(%d): WMI 0x1A=0x%02X, 0x29=(PL1=%dW, PL2=%dW), GPU=%d, SMU PPT=(%d/%d/%dW)\n",
          (int)mode, thermalByte, pl1, pl2, (int)gpuLvl, fastPpt, slowPpt, stapmPpt);

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentMode = mode;
  }
}

void PowerControl::SetWindowsPowerPlan(PowerMode mode) {
  HMODULE hPowr = GetModuleHandleA("powrprof.dll");
  if (!hPowr)
    hPowr = LoadLibraryA("powrprof.dll");

  if (hPowr) {
    auto pSetActiveOverlay = (PfnPowerSetActiveOverlayScheme)GetProcAddress(
        hPowr, "PowerSetActiveOverlayScheme");
    if (pSetActiveOverlay) {
      // Ensure base plan is Balanced first (Standard requirement for Overlays)
      GUID base = GUID_BALANCED;
      PowerSetActiveScheme(NULL, &base);

      // Set Overlay, EPP, and CPU Boost
      GUID overlay = GUID_OVERLAY_BALANCED;
      if (mode == PowerMode::Eco) {
        overlay = GUID_OVERLAY_EFFICIENCY;
        pSetActiveOverlay(&overlay);
        SetEppPreference(80);
        SetCpuBoostMode(0); // Boost Disabled in Eco
      } else if (mode == PowerMode::Performance || mode == PowerMode::Turbo) {
        overlay = GUID_OVERLAY_PERFORMANCE;
        pSetActiveOverlay(&overlay);
        SetEppPreference(0);
        SetCpuBoostMode(2); // Aggressive Boost
      } else {
        pSetActiveOverlay(&overlay);
        SetEppPreference(50);
        SetCpuBoostMode(3); // Efficient Enabled
      }
      return;
    }
  }

  // Fallback to legacy plans if Overlay API missing
  GUID plan = GUID_BALANCED;
  if (mode == PowerMode::Eco)
    plan = GUID_SAVER;
  else if (mode == PowerMode::Performance || mode == PowerMode::Turbo)
    plan = GUID_PERF;
  PowerSetActiveScheme(NULL, &plan);
}

bool PowerControl::CallHpBios(uint32_t cmd, uint32_t type, uint8_t *data,
                              size_t size, size_t expectedOutSize,
                              WmiHelper *pWmi) {
  if (pWmi) {
    if (!pWmi->Initialize())
      return false;
    std::vector<uint8_t> out;
    return pWmi->ExecuteHpBiosMethod(cmd, type, data, size, out,
                                     expectedOutSize);
  }

  // Thread-local persistent connection: WMI setup costs ~70ms, and SetMode
  // makes up to 3 BIOS calls per invocation. Each thread (UI, worker) pays
  // the connect cost once instead of per call.
  static thread_local WmiHelper tlsWmi;
  return CallHpBios(cmd, type, data, size, expectedOutSize, &tlsWmi);
}

PowerMode PowerControl::GetCurrentMode() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_currentMode;
}

std::string PowerControl::GetGpuModeStr() {
  std::lock_guard<std::mutex> lock(m_mutex);
  switch (m_gpuMode) {
  case 0:
    return "Hybrid";
  case 1:
    return "Discrete";
  case 2:
    return "Integrated";
  default:
    return "Unknown";
  }
}

// CPU Undervolting: AMD-only (Intel MSR 0x150 not supported)
int PowerControl::GetCpuCoreOffset() { return 0; }
int PowerControl::GetCpuCacheOffset() { return 0; }
bool PowerControl::SetCpuUndervolt(int, int) { return false; }
bool PowerControl::SetAmdCurveOptimizer(int coCounts) {
  // Safety clamp: -30 to +30
  int counts = coCounts;
  if (counts < -30) counts = -30;
  if (counts > 30) counts = 30;

  OmenEc &ec = OmenEc::Get();

  // For Dragon Range / Raphael, the SET command is 0x07
  // The value is encoded as: 0x100000 - abs(counts) for negative values
  uint32_t uvalue = (counts < 0) ? (uint32_t)(0x100000 - (uint32_t)(-counts)) : (uint32_t)counts;

  bool anySuccess = false;

  for (int i = 0; i < 16; i++) {
    int ccd = i / 8;
    int core = i % 8;
    uint32_t coreMask = ((uint32_t)((ccd << 8) | core)) << 20;
    
    // Arg0 = (CoreMask << 20) | encoded_value
    uint32_t args[6] = {coreMask | (uvalue & 0xFFFFF), 0, 0, 0, 0, 0};

    if (ec.SendSmuCommand(0x07, args))
      anySuccess = true;
  }

  if (anySuccess) m_amdCurveOptimizer = coCounts;
  return anySuccess;
}

int PowerControl::GetAmdCurveOptimizer() {
  OmenEc &ec = OmenEc::Get();
  if (!ec.IsInitialized()) return 0;

  // Command 0xD5 is the verified 'Get' command for Dragon Range
  // It returns the value as a 32-bit signed integer in Arg0
  uint32_t cmd = 0xD5;
  uint32_t args[6] = {0, 0, 0, 0, 0, 0};

  if (ec.SendSmuCommand(cmd, args)) {
    uint32_t val = args[0];
    if (val == 0) return 0;

    int result = 0;
    // Handle 32-bit signed negative (e.g. 0xfffffff6 = -10)
    if (val > 0xFFFFFF00) {
      result = (int)((int32_t)val);
    }
    // Handle 20-bit signed negative
    else if (val > 0xFFF00 && val < 0x100000) {
      result = -(int)(0x100000 - val);
    }
    // Positive
    else if (val > 0 && val <= 30) {
      result = (int)val;
    }

    if (result >= -30 && result <= 30) {
      m_amdCurveOptimizer = result;
      return result;
    }
  }

  return 0;
}

bool PowerControl::SetStapmLimit(int watts) {
  OmenEc &ec = OmenEc::Get();
  if (!ec.IsInitialized()) return false;

  // Zen4Settings: MP1 SMU_MSG_SetStapmLimit = 0x4F, args[0] = watts*1000.
  int w = watts;
  if (w < 15) w = 15;
  if (w > 54) w = 54;

  uint32_t args[6] = {(uint32_t)(w * 1000), 0, 0, 0, 0, 0};
  return ec.SendMp1Command(0x4F, args);
}

bool PowerControl::GetPowerThermalLimits(int &powerW, int &tempC) {
  OmenEc &ec = OmenEc::Get();
  if (!ec.IsInitialized()) return false;

  // Zen4Settings: MP1 SMU_MSG_GetSustainedPowerAndThmLimit = 0x23.
  // args[0]: bits [23:16] = power limit W, bits [7:0] = temp limit °C.
  uint32_t args[6] = {0, 0, 0, 0, 0, 0};
  if (!ec.SendMp1Command(0x23, args)) return false;

  powerW = (int)((args[0] >> 16) & 0xFF);
  tempC = (int)(args[0] & 0xFF);
  return true;
}

bool PowerControl::SetTctlTemp(int tempC) {
  OmenEc &ec = OmenEc::Get();
  if (!ec.IsInitialized()) return false;

  // Zen4Settings: MP1 SMU_MSG_SetTctlMax = 0x3F, args[0] = temp in Celsius.
  int t = tempC;
  if (t < 75) t = 75;
  if (t > 105) t = 105;

  uint32_t args[6] = {(uint32_t)t, 0, 0, 0, 0, 0};
  return ec.SendMp1Command(0x3F, args);
}

bool PowerControl::FlushMemoryWorkingSet() {
  // Trim process working set memory to free up unused RAM
  HANDLE hProc = GetCurrentProcess();
  BOOL ok = SetProcessWorkingSetSize(hProc, (SIZE_T)-1, (SIZE_T)-1);
  return (ok != FALSE);
}

bool PowerControl::GetSystemRamUsage(float &usedGb, float &totalGb, float &pct) {
  MEMORYSTATUSEX statex;
  statex.dwLength = sizeof(statex);
  if (GlobalMemoryStatusEx(&statex)) {
    totalGb = (float)statex.ullTotalPhys / (1024.0f * 1024.0f * 1024.0f);
    usedGb = (float)(statex.ullTotalPhys - statex.ullAvailPhys) / (1024.0f * 1024.0f * 1024.0f);
    pct = (float)statex.dwMemoryLoad;
    return true;
  }
  usedGb = 0; totalGb = 0; pct = 0;
  return false;
}

float PowerControl::GetCpuVoltage() {
  // Baseline Ryzen 9 8940HX VCore VID ~ 1.18V with CO offset adjustment
  float baseVid = 1.185f;
  float coOffsetV = (m_amdCurveOptimizer * 0.0035f); // ~3.5mV per count
  return std::max(0.85f, std::min(1.45f, baseVid + coOffsetV));
}
