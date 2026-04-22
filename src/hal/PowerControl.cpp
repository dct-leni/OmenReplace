#include "PowerControl.h"
#include "FanService.h"
#include "NvApiHelper.h"
#include "OmenEc.h"
#include "WmiHelper.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <powrprof.h>

#pragma comment(lib, "powrprof.lib")

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

// Function Pointers for Hidden APIs
typedef DWORD(WINAPI *PfnPowerSetActiveOverlayScheme)(GUID *);
typedef DWORD(WINAPI *PfnPowerGetActualOverlayScheme)(GUID *);

PowerControl &PowerControl::Get() {
  static PowerControl instance;
  return instance;
}

PowerControl::PowerControl() { Update(); }

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

  // 2. Read Windows Power Plan
  GUID *activePlan = NULL;
  PowerMode detectedWin = detectedEc;
  if (PowerGetActiveScheme(NULL, &activePlan) == ERROR_SUCCESS) {
    if (IsEqualGUID(*activePlan, GUID_SAVER))
      detectedWin = PowerMode::Eco;
    else if (IsEqualGUID(*activePlan, GUID_BALANCED))
      detectedWin = PowerMode::Balanced;
    else if (IsEqualGUID(*activePlan, GUID_PERF))
      detectedWin = PowerMode::Performance;
    LocalFree(activePlan);
  }

  // 3. Update Graphics Mode
  std::vector<uint8_t> out;

  // Use persistent WMI helper for background thread
  if (m_wmiBg.Initialize()) {
    // GPU Mode: Use 0x00001 (Legacy) and 0x52
    if (m_wmiBg.ExecuteHpBiosMethod(0x00001, 0x52, NULL, 0, out, 4)) {
      if (out.size() > 0) {
        if (m_gpuMode != out[0]) {
          if (out[0] <= 2) {
            m_gpuMode = out[0];
          }
        }
      }

      // 4. Update Windows Overlay Scheme
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
            // Note: If overlay is unknown or NULL, we assume Balanced or ignore
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
  // level: 0=Min, 1=Med, 2=Max
  uint8_t data[4] = {0};
  // GpuCustomTgp, GpuPpab, GpuDState, PeakTemperature
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
  }
  data[2] = 0x01; // DState = D1
  data[3] = 0x00; // PeakTemp

  return CallHpBios(0x20008, 0x22, data, 4, 0);
}

bool PowerControl::SetGpuMode(int mode) {
  // 0=Hybrid, 1=Discrete
  // BiosCmd.GpuMode = 0x00002
  // CMD_GPU_SET_MODE = 0x52
  // Data: 4 bytes, [0]=mode

  uint8_t data[4] = {(uint8_t)mode, 0, 0, 0};
  bool success = CallHpBios(0x00002, 0x52, data, 4, 0);

  return success;
}

bool PowerControl::ExtendFanCountdown() {
  uint8_t data[4] = {0x1E, 0, 0, 0};
  // Always use background WMI helper since this is only called from background
  // thread
  return CallHpBios(0x20008, 0x31, data, 4, 0, &m_wmiBg);
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

bool PowerControl::GetFanLevelWmi(int &cpuLevel, int &gpuLevel) {
  // CMD_FAN_GET_LEVEL = 0x2D
  WmiHelper wmi;
  if (!wmi.Initialize())
    return false;

  std::vector<uint8_t> out;
  if (wmi.ExecuteHpBiosMethod(0x20008, 0x2D, NULL, 0, out, 128)) {
    if (out.size() >= 2) {
      cpuLevel = out[0];
      gpuLevel = out[1];
      return true;
    }
  }
  return false;
}

void PowerControl::RequestGpuMode(int mode) {
  if (GetGpuModeInt() == mode)
    return;
  // Usually WMI CMD 0x20008, type 0x3E, data { mode }
  uint8_t d[4] = {(uint8_t)mode};
  CallHpBios(0x20008, 0x3E, d, 4, 0);
  m_gpuMode = mode;
}

PowerControl::GpuOverclockSettings PowerControl::GetGpuOverclock() {
  GpuOverclockSettings s;

  // Keep a persistent NvApiHelper in a static to avoid reinitializing per frame
  // This is safe because PowerControl::GetGpuOverclock is only called from UI
  static NvApiHelper nvapi;
  static bool nvInit = false;
  if (!nvInit) {
    s.isNvidia = nvapi.Initialize();
    nvInit = true;
  } else {
    s.isNvidia = nvapi.IsInitialized();
  }

  s.isSupported = s.isNvidia;

  if (s.isNvidia) {
    // Read real clock offsets
    nvapi.GetClockOffsets(s.coreClockOffset, s.memoryClockOffset);

    // Read real power limit
    nvapi.GetPowerLimitPercent(s.powerLimitPercent);

    // Read range for sliders
    nvapi.GetPowerLimitRange(s.pwrMin, s.pwrMax);
    // Core/mem offset range is ±500/+1000 by convention (Afterburner standard)
    s.coreMin = -500;
    s.coreMax = 500;
    s.memMin = -500;
    s.memMax = 1000;
  }

  return s;
}

bool PowerControl::SetGpuOverclock(const GpuOverclockSettings &settings) {
  static NvApiHelper nvapi;
  static bool nvInit = false;
  if (!nvInit)
    nvInit = nvapi.Initialize();

  if (!nvapi.IsInitialized())
    return false;

  bool ok = true;
  ok &= nvapi.SetClockOffsets(settings.coreClockOffset,
                              settings.memoryClockOffset);
  ok &= nvapi.SetPowerLimitPercent(settings.powerLimitPercent);
  return ok;
}

int PowerControl::GetBatteryChargeLimit() {
  // WMI only tells us enabled(0x01) or disabled(0x00).
  // If disabled -> 100%. If enabled -> return saved custom percentage (or 80
  // default).
  uint8_t inData[4] = {0, 0, 0, 0};
  WmiHelper wmi;
  if (!wmi.Initialize())
    return m_batteryLimitPercent; // return cached value on WMI failure

  auto &oc = FanService::Get().GetOverlayConfig();
  int currentLimit = oc.batteryLimit;
  
  // BIOS masks battery read values on many firmwares (returning empty arrays).
  // We trust the locally persisted user config set by APPLY as the solitary truth.
  if (currentLimit < 60 || currentLimit > 100) {
      currentLimit = 100; // Default to standard 100% disabled
      oc.batteryLimit = currentLimit;
  }

  return currentLimit;
}

bool PowerControl::SetBatteryChargeLimit(int limitPercent) {
  limitPercent = std::max(60, std::min(100, limitPercent));

  uint8_t data[4] = {0};
  // Send exact limit percentage to BIOS (100 = disabled essentially, or
  // specific disable command)
  if (limitPercent >= 100) {
    data[0] = 0x00; // Disable battery care
  } else {
    data[0] = (uint8_t)limitPercent; // BIOS accepts custom percentage (60-100)
  }

  auto &oc = FanService::Get().GetOverlayConfig();
  oc.batteryLimit = limitPercent;
  FanService::Get().SaveConfig();

  WmiHelper wmi;
  if (!wmi.Initialize())
    return false;
  std::vector<uint8_t> out;
  // Use expectedOutSize=0 for write commands
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

void PowerControl::SetMode(PowerMode mode) {
  CheckThermalPolicy();

  uint8_t ecValue = 0x00;
  uint8_t biosValue = 0x30; // Default V1
  uint8_t gpuLevel = 1;     // Medium

  switch (mode) {
  case PowerMode::Eco:
    ecValue = 0x02;
    biosValue = (m_thermalPolicy == ThermalPolicyVersion::V0_Legacy)
                    ? 0x02
                    : 0x50; // Cool
    gpuLevel = 0;           // Min
    break;
  case PowerMode::Balanced:
    ecValue = 0x00;
    biosValue = (m_thermalPolicy == ThermalPolicyVersion::V0_Legacy)
                    ? 0x00
                    : 0x30; // Default
    gpuLevel = 1;           // Med
    break;
  case PowerMode::Performance:
  case PowerMode::Turbo:
    ecValue = 0x01;
    biosValue = (m_thermalPolicy == ThermalPolicyVersion::V0_Legacy)
                    ? 0x01
                    : 0x31; // Perf
    gpuLevel = 2;           // Max
    break;
  }

  // 1. Write to EC (Direct)
  OmenEc::Get().WriteByte(0xCE, ecValue);

  // 3. Set GPU Power (BIOS)
  SetGpuPower(gpuLevel);

  // 4. Set Windows Power Plan (Overlay)
  SetWindowsPowerPlan(mode);

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
      // Ensure base plan is Balanced first (Standard requirement for
      // Overlays)
      GUID base = GUID_BALANCED;
      PowerSetActiveScheme(NULL, &base);

      // Set Overlay
      GUID overlay = GUID_OVERLAY_BALANCED;
      if (mode == PowerMode::Eco)
        overlay = GUID_OVERLAY_EFFICIENCY;
      else if (mode == PowerMode::Performance || mode == PowerMode::Turbo)
        overlay = GUID_OVERLAY_PERFORMANCE;

      DWORD res = pSetActiveOverlay(&overlay);
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

  WmiHelper wmi;
  if (!wmi.Initialize())
    return false;
  std::vector<uint8_t> out;
  return wmi.ExecuteHpBiosMethod(cmd, type, data, size, out, expectedOutSize);
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

// CPU Undervolting Implementation (Intel MSR 0x150)
int PowerControl::GetCpuCoreOffset() {
  // Placeholder: This requires reading MSR 0x150 with command 0x10 and Plane 0
  return 0;
}

int PowerControl::GetCpuCacheOffset() {
  // Placeholder: This requires reading MSR 0x150 with command 0x10 and Plane 2
  return 0;
}

bool PowerControl::SetCpuUndervolt(int coreMv, int cacheMv) {
  // MSR 0x150 is the OC Mailbox
  // Bit 63: Write(1)
  // Bits 47:40: Plane (0=Core, 2=Cache)
  // Bits 39:32: Command (0x11=Write)
  // Bits 31:21: Offset (mV * 1024 / 1000, two's complement)

  // Implementation requires OmenEc's PawnIO instance to load IntelMSR.bin
  return false; // Not yet fully implemented due to driver setup
}
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

