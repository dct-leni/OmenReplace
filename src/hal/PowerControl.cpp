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

  static bool s_firstUpdate = true;
  if (s_firstUpdate) {
    s_firstUpdate = false;
    return;
  }

  // Synchronize
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_currentMode == PowerMode::Turbo) {
    // In Turbo mode, Windows overlay and EC byte 0xCE naturally match Performance mode.
    // Only leave Turbo if the user actively selects Eco or Balanced.
    if (detectedOverlay == PowerMode::Eco || detectedOverlay == PowerMode::Balanced) {
      m_currentMode = detectedOverlay;
    } else if (detectedEc == PowerMode::Eco || detectedEc == PowerMode::Balanced) {
      m_currentMode = detectedEc;
    }
    return;
  }

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

// ─── BIOS WMI Setting Helper (root\HP\InstrumentedBIOS) ──────────────────────
static bool SetBiosSettingWmi(const std::wstring &settingName, const std::wstring &valueStr) {
  IWbemLocator *pLoc = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, (LPVOID *)&pLoc);
  if (FAILED(hr)) return false;

  IWbemServices *pSvc = nullptr;
  BSTR path = SysAllocString(L"root\\HP\\InstrumentedBIOS");
  hr = pLoc->ConnectServer(path, NULL, NULL, NULL, 0, NULL, NULL, &pSvc);
  SysFreeString(path);

  if (FAILED(hr)) {
    pLoc->Release();
    return false;
  }

  // Set Packet Privacy security on the proxy
  CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                    RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
                    NULL, EOAC_NONE);

  IWbemClassObject *pClass = nullptr;
  BSTR clsName = SysAllocString(L"HP_BIOSSettingInterface");
  hr = pSvc->GetObject(clsName, 0, NULL, &pClass, NULL);
  SysFreeString(clsName);

  bool ok = false;
  if (SUCCEEDED(hr) && pClass) {
    IWbemClassObject *pInParamsDef = nullptr;
    BSTR methName = SysAllocString(L"SetBIOSSetting");
    hr = pClass->GetMethod(methName, 0, &pInParamsDef, NULL);
    if (SUCCEEDED(hr) && pInParamsDef) {
      IWbemClassObject *pInInstance = nullptr;
      hr = pInParamsDef->SpawnInstance(0, &pInInstance);
      if (SUCCEEDED(hr) && pInInstance) {
        VARIANT vtName, vtVal, vtPass;
        vtName.vt = VT_BSTR; vtName.bstrVal = SysAllocString(settingName.c_str());
        vtVal.vt = VT_BSTR; vtVal.bstrVal = SysAllocString(valueStr.c_str());
        vtPass.vt = VT_BSTR; vtPass.bstrVal = SysAllocString(L"");

        pInInstance->Put(L"Name", 0, &vtName, 0);
        pInInstance->Put(L"Value", 0, &vtVal, 0);
        pInInstance->Put(L"Password", 0, &vtPass, 0);

        VariantClear(&vtName);
        VariantClear(&vtVal);
        VariantClear(&vtPass);

        IWbemClassObject *pOutParams = nullptr;
        BSTR instPath = SysAllocString(L"HP_BIOSSettingInterface");
        hr = pSvc->ExecMethod(instPath, methName, 0, NULL, pInInstance, &pOutParams, NULL);
        SysFreeString(instPath);

        if (SUCCEEDED(hr) && pOutParams) {
          VARIANT vtRet;
          VariantInit(&vtRet);
          if (SUCCEEDED(pOutParams->Get(L"Return", 0, &vtRet, NULL, NULL))) {
            int retCode = (vtRet.vt == VT_I4) ? vtRet.lVal : (int)vtRet.uintVal;
            ok = (retCode == 0);
            OmenLog("[AMDOMEN] SetBIOSSetting(%ls, %ls) returned %d\n",
                    settingName.c_str(), valueStr.c_str(), retCode);
            VariantClear(&vtRet);
          }
          pOutParams->Release();
        }
        pInInstance->Release();
      }
      pInParamsDef->Release();
    }
    SysFreeString(methName);
    pClass->Release();
  }

  pSvc->Release();
  pLoc->Release();
  return ok;
}

// ─── Wake-on-LAN (Wired NIC Magic Packet + HP BIOS S3/S4/S5) ─────────────────
bool PowerControl::GetWakeOnLan() {
  if (m_wolCached >= 0)
    return m_wolCached == 1;

  auto &oc = FanService::Get().GetOverlayConfig();
  m_wolCached = oc.wakeOnLan ? 1 : 0;
  return oc.wakeOnLan;
}

bool PowerControl::SetWakeOnLan(bool enable) {
  // 1. Windows Network Adapter Driver Registry Settings
  HKEY hClassKey = NULL;
  bool anyModified = false;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}",
                    0, KEY_READ | KEY_WRITE, &hClassKey) == ERROR_SUCCESS) {
    const wchar_t *targetVal = enable ? L"1" : L"0";
    DWORD targetBytes = (DWORD)((wcslen(targetVal) + 1) * sizeof(wchar_t));
    DWORD pnpCap = enable ? 0 : 0x100;

    for (DWORD i = 0; i < 64; i++) {
      wchar_t subKeyName[32] = {0};
      DWORD subKeyLen = 32;
      if (RegEnumKeyExW(hClassKey, i, subKeyName, &subKeyLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        break;

      HKEY hSub = NULL;
      if (RegOpenKeyExW(hClassKey, subKeyName, 0, KEY_READ | KEY_WRITE, &hSub) == ERROR_SUCCESS) {
        wchar_t existing[16] = {0};
        DWORD existingSize = sizeof(existing);
        DWORD type = 0;
        if (RegQueryValueExW(hSub, L"*WakeOnMagicPacket", nullptr, &type, (LPBYTE)existing, &existingSize) == ERROR_SUCCESS) {
          RegSetValueExW(hSub, L"*WakeOnMagicPacket", 0, REG_SZ, (const BYTE *)targetVal, targetBytes);
          RegSetValueExW(hSub, L"*WakeOnPattern", 0, REG_SZ, (const BYTE *)targetVal, targetBytes);
          RegSetValueExW(hSub, L"EnablePME", 0, REG_SZ, (const BYTE *)targetVal, targetBytes);
          RegSetValueExW(hSub, L"WakeOnSlot", 0, REG_SZ, (const BYTE *)targetVal, targetBytes);
          RegSetValueExW(hSub, L"PnPCapabilities", 0, REG_DWORD, (const BYTE *)&pnpCap, sizeof(pnpCap));
          anyModified = true;
        }
        RegCloseKey(hSub);
      }
    }
    RegCloseKey(hClassKey);
  }

  // 2. HP BIOS S3/S4/S5 Wake on LAN
  const wchar_t *valStr = enable ? L"Enable" : L"Disable";
  SetBiosSettingWmi(L"S3/S4/S5 Wake on LAN", valStr);
  SetBiosSettingWmi(L"S4/S5 Wake on LAN", valStr);
  SetBiosSettingWmi(L"Wake on LAN", valStr);
  SetBiosSettingWmi(L"LAN Wake From DeepSx", valStr);

  auto &cfg = FanService::Get().GetOverlayConfig();
  cfg.wakeOnLan = enable;
  FanService::Get().SaveConfig();

  m_wolCached = enable ? 1 : 0;
  return anyModified;
}

// ─── Wake-on-WLAN & Bluetooth (Wireless M.2 Magic Packet + HP BIOS Wake on WLAN/BT) ───
bool PowerControl::GetWakeOnWlanBt() {
  if (m_wlanBtWolCached >= 0)
    return m_wlanBtWolCached == 1;

  auto &oc = FanService::Get().GetOverlayConfig();
  m_wlanBtWolCached = oc.wakeOnWlanBt ? 1 : 0;
  return oc.wakeOnWlanBt;
}

bool PowerControl::SetWakeOnWlanBt(bool enable) {
  // 1. HP BIOS Wake on WLAN and BT Enable
  const wchar_t *valStr = enable ? L"Enable" : L"Disable";
  SetBiosSettingWmi(L"Wake on WLAN and BT Enable", valStr);
  SetBiosSettingWmi(L"DeepSx Wake on WLAN and BT Enable", valStr);

  // 2. Wireless Adapter Registry Wake properties
  HKEY hClassKey = NULL;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}",
                    0, KEY_READ | KEY_WRITE, &hClassKey) == ERROR_SUCCESS) {
    const wchar_t *targetVal = enable ? L"1" : L"0";
    DWORD targetBytes = (DWORD)((wcslen(targetVal) + 1) * sizeof(wchar_t));
    DWORD pnpCap = enable ? 0 : 0x100;

    for (DWORD i = 0; i < 64; i++) {
      wchar_t subKeyName[32] = {0};
      DWORD subKeyLen = 32;
      if (RegEnumKeyExW(hClassKey, i, subKeyName, &subKeyLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        break;

      HKEY hSub = NULL;
      if (RegOpenKeyExW(hClassKey, subKeyName, 0, KEY_READ | KEY_WRITE, &hSub) == ERROR_SUCCESS) {
        wchar_t driverDesc[128] = {0};
        DWORD descSize = sizeof(driverDesc);
        DWORD type = 0;
        if (RegQueryValueExW(hSub, L"DriverDesc", nullptr, &type, (LPBYTE)driverDesc, &descSize) == ERROR_SUCCESS) {
          if (wcsstr(driverDesc, L"Wi-Fi") || wcsstr(driverDesc, L"Wireless") || wcsstr(driverDesc, L"WLAN") ||
              wcsstr(driverDesc, L"802.11") || wcsstr(driverDesc, L"Bluetooth")) {
            RegSetValueExW(hSub, L"*WakeOnMagicPacket", 0, REG_SZ, (const BYTE *)targetVal, targetBytes);
            RegSetValueExW(hSub, L"*WakeOnPattern", 0, REG_SZ, (const BYTE *)targetVal, targetBytes);
            RegSetValueExW(hSub, L"PnPCapabilities", 0, REG_DWORD, (const BYTE *)&pnpCap, sizeof(pnpCap));
          }
        }
        RegCloseKey(hSub);
      }
    }
    RegCloseKey(hClassKey);
  }

  auto &cfg = FanService::Get().GetOverlayConfig();
  cfg.wakeOnWlanBt = enable;
  FanService::Get().SaveConfig();

  m_wlanBtWolCached = enable ? 1 : 0;
  return true;
}

// ─── AC-line auto-switch ───────────────────────────────────────────────────
// On battery: save current mode/profile, apply Eco + Quiet.
// On AC: restore what was saved. Only acts on transitions.
void PowerControl::CheckAcLine() {
  if (!m_acEnabled)
    return;

  SYSTEM_POWER_STATUS st;
  if (!GetSystemPowerStatus(&st))
    return;
  if (st.ACLineStatus != 0 && st.ACLineStatus != 1)
    return; // unknown
  if (st.BatteryFlag == 128)
    return; // no battery — always treat as AC, no switching

  int line = st.ACLineStatus;
  if (m_acLastLine == -1) {
    m_acLastLine = line; // first observation, no transition
    return;
  }
  if (line == m_acLastLine)
    return;
  m_acLastLine = line;

  if (line == 0) {
    // Unplugged → save + Eco/Quiet.
    m_acSavedMode = GetCurrentMode();
    m_acSavedProfile = (int)FanService::Get().GetProfile();
    m_acSaved = true;
    OmenLog("[AMDOMEN] ac_switch: on battery -> eco (saved mode=%d profile=%d)\n",
            (int)m_acSavedMode, m_acSavedProfile);
    SetMode(PowerMode::Eco);
    FanService::Get().SetControlMode(FanControlMode::AppMode);
    FanService::Get().SetProfile(FanControlProfile::Quiet);
  } else {
    // Plugged back in → restore.
    if (m_acSaved) {
      OmenLog("[AMDOMEN] ac_switch: on AC -> restore mode=%d profile=%d\n",
              (int)m_acSavedMode, m_acSavedProfile);
      SetMode(m_acSavedMode);
      FanService::Get().SetControlMode(FanControlMode::AppMode);
      FanService::Get().SetProfile((FanControlProfile)m_acSavedProfile);
      m_acSaved = false;
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

bool PowerControl::SetFanMax(bool enabled) {
  // CMD_FAN_MAX_SET = 0x27
  uint8_t data[4] = {(uint8_t)(enabled ? 1 : 0), 0, 0, 0};
  bool ok = CallHpBios(0x20008, 0x27, data, 4, 0, &m_wmiBg);
  if (ok) {
    m_maxFanActive = enabled;
  }
  return ok;
}

bool PowerControl::SetFanLevelWmiBg(int cpuPercent, int gpuPercent) {
  // HP BIOS Max Fan Mode (0x27) unlocks the absolute hardware maximum fan speed (5500-6000 RPM)
  if (cpuPercent >= 95 || gpuPercent >= 95) {
    if (!m_maxFanActive) {
      SetFanMax(true);
    }
  } else {
    if (m_maxFanActive) {
      SetFanMax(false);
    }
  }

  // CMD_FAN_SET_LEVEL = 0x2E
  // Valid BIOS level range: 0 to 55 (55 = 5500 RPM nominal)
  uint8_t cpuLevel = (uint8_t)(std::clamp(cpuPercent, 0, 100) * 55 / 100);
  uint8_t gpuLevel = (uint8_t)(std::clamp(gpuPercent, 0, 100) * 55 / 100);
  uint8_t data[4] = {cpuLevel, gpuLevel, 0, 0};

  // Use persistent background WMI helper
  return CallHpBios(0x20008, 0x2E, data, 4, 0, &m_wmiBg);
}

bool PowerControl::SetFanLevelWmi(int cpuPercent, int gpuPercent) {
  if (cpuPercent >= 95 || gpuPercent >= 95) {
    if (!m_maxFanActive) {
      SetFanMax(true);
    }
  } else {
    if (m_maxFanActive) {
      SetFanMax(false);
    }
  }

  // CMD_FAN_SET_LEVEL = 0x2E
  // Valid BIOS level range: 0 to 55 (55 = 5500 RPM nominal)
  uint8_t cpuLevel = (uint8_t)(std::clamp(cpuPercent, 0, 100) * 55 / 100);
  uint8_t gpuLevel = (uint8_t)(std::clamp(gpuPercent, 0, 100) * 55 / 100);
  uint8_t data[4] = {cpuLevel, gpuLevel, 0, 0};

  return CallHpBios(0x20008, 0x2E, data, 4, 0);
}

void PowerControl::RequestGpuMode(int mode) {
  // BiosCmd.GpuMode = 0x00002, CMD_GPU_SET_MODE = 0x52 (per OmenMon / omencore)
  uint8_t d[4] = {(uint8_t)mode, 0, 0, 0};
  bool ok = CallHpBios(0x00002, 0x52, d, 4, 0);
  OmenLog("[AMDOMEN] RequestGpuMode(%d) via WMI 0x00002,0x52: %s (requires reboot)\n",
          mode, ok ? "SUCCESS" : "FAILED");
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
  bool ok = wmi.ExecuteHpBiosMethod(0x20008, 0x24, data, 4, out, 0);
  OmenLog("[AMDOMEN] SetBatteryChargeLimit(%d%%) -> WMI 0x24 data[0]=0x%02X result=%s\n",
          normalizedLimit, data[0], ok ? "OK" : "FAILED");
  return ok;
}

bool PowerControl::GetDisplayOverdrive() {
  auto &oc = FanService::Get().GetOverlayConfig();
  return oc.displayOverdrive;
}

bool PowerControl::ReadHardwareDisplayOverdrive() {
  WmiHelper wmi;
  if (!wmi.Initialize()) return m_displayOverdrive;

  uint8_t data[4] = {0};
  std::vector<uint8_t> out;
  // CMD_OVERDRIVE_GET = 0x35
  if (wmi.ExecuteHpBiosMethod(0x20008, 0x35, data, 4, out, 4) && !out.empty()) {
    bool enabled = (out[0] != 0);
    m_displayOverdrive = enabled;
    auto &oc = FanService::Get().GetOverlayConfig();
    oc.displayOverdrive = enabled;
    return enabled;
  }
  return m_displayOverdrive;
}

bool PowerControl::SetDisplayOverdrive(bool enable) {
  m_displayOverdrive = enable;
  auto &oc = FanService::Get().GetOverlayConfig();
  oc.displayOverdrive = enable;
  FanService::Get().SaveConfig();

  uint8_t data[4] = {(uint8_t)(enable ? 1 : 0), 0, 0, 0};
  WmiHelper wmi;
  if (!wmi.Initialize()) return false;
  std::vector<uint8_t> out;
  // CMD_OVERDRIVE_SET = 0x36
  bool ok = wmi.ExecuteHpBiosMethod(0x20008, 0x36, data, 4, out, 0);
  OmenLog("[AMDOMEN] SetDisplayOverdrive(%s) -> WMI 0x36 result=%s\n",
          enable ? "ON" : "OFF", ok ? "OK" : "FAILED");
  return ok;
}

void PowerControl::RestoreFanAuto() {
  // 1. Disable Max Fan via BIOS method 0x27
  m_maxFanActive = false;
  uint8_t maxData[4] = {0x00, 0x00, 0x00, 0x00};
  CallHpBios(0x20008, 0x27, maxData, 4, 0);

  // 2. Reset the idle watchdog/manual extension (0x31)
  uint8_t idleData[4] = {0x00, 0x00, 0x00, 0x00};
  CallHpBios(0x20008, 0x31, idleData, 4, 0);
  OmenLog("[AMDOMEN] RestoreFanAuto: Reset MaxFan 0x27 & Watchdog 0x31\n");
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
  // Manual TGP override (pills) wins over the mode table until set back to Auto.
  if (m_gpuPowerOverride >= 0 && m_gpuPowerOverride <= 2)
    SetGpuPower((uint8_t)m_gpuPowerOverride);

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

  PfnPowerSetActiveOverlayScheme pSetActiveOverlay = nullptr;
  if (hPowr) {
    pSetActiveOverlay = (PfnPowerSetActiveOverlayScheme)GetProcAddress(
        hPowr, "PowerSetActiveOverlayScheme");
  }

  GUID *activeScheme = nullptr;
  if (PowerGetActiveScheme(NULL, &activeScheme) == ERROR_SUCCESS && activeScheme) {
    DWORD eppVal = 50;
    DWORD boostMode = 3;
    GUID overlay = GUID_OVERLAY_BALANCED;

    if (mode == PowerMode::Eco) {
      overlay = GUID_OVERLAY_EFFICIENCY;
      eppVal = 80;
      boostMode = 0; // Disabled
    } else if (mode == PowerMode::Performance || mode == PowerMode::Turbo) {
      overlay = GUID_OVERLAY_PERFORMANCE;
      eppVal = 0;
      boostMode = 2; // Aggressive
    }

    if (pSetActiveOverlay) {
      pSetActiveOverlay(&overlay);
    }
    PowerWriteACValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFEPP, eppVal);
    PowerWriteDCValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFEPP, eppVal);
    PowerWriteACValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFBOOST, boostMode);
    PowerWriteDCValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PERFBOOST, boostMode);
    // Single atomic activation to prevent DWM / mouse stutter
    PowerSetActiveScheme(NULL, activeScheme);
    LocalFree(activeScheme);
  }
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

PowerMode PowerControl::ReadHardwarePowerMode() {
  uint8_t mode = OmenEc::Get().ReadByte(0xCE);
  if (mode == 0x00 || mode == 0x30) return PowerMode::Balanced;
  if (mode == 0x01 || mode == 0x31) return PowerMode::Performance;
  if (mode == 0x02 || mode == 0x50 || mode == 0x03) return PowerMode::Eco;

  HMODULE hPowr = GetModuleHandleA("powrprof.dll");
  if (!hPowr) hPowr = LoadLibraryA("powrprof.dll");
  if (hPowr) {
    auto pGetActualOverlay = (PfnPowerGetActualOverlayScheme)GetProcAddress(
        hPowr, "PowerGetActualOverlayScheme");
    if (pGetActualOverlay) {
      GUID overlay;
      if (pGetActualOverlay(&overlay) == ERROR_SUCCESS) {
        if (IsEqualGUID(overlay, GUID_OVERLAY_EFFICIENCY)) return PowerMode::Eco;
        if (IsEqualGUID(overlay, GUID_OVERLAY_PERFORMANCE)) return PowerMode::Performance;
        if (IsEqualGUID(overlay, GUID_OVERLAY_BALANCED)) return PowerMode::Balanced;
      }
    }
  }
  return PowerMode::Balanced;
}

int PowerControl::ReadHardwareGpuPower() {
  return GetEffectiveGpuPowerLevel();
}

int PowerControl::ReadHardwareAmdCurveOptimizer() {
  return GetAmdCurveOptimizer();
}

int PowerControl::ReadHardwareTctlLimit() {
  int pw = 0, tc = 0;
  if (GetPowerThermalLimits(pw, tc)) {
    return tc;
  }
  return 0;
}

int PowerControl::ReadHardwareBatteryLimit() {
  WmiHelper wmi;
  if (wmi.Initialize()) {
    uint8_t data[4] = {0};
    std::vector<uint8_t> out;
    if (wmi.ExecuteHpBiosMethod(0x20008, 0x24, data, 4, out, 4) && !out.empty()) {
      int readLimit = (out[0] == 0x01) ? 80 : 100;
      m_batteryLimitPercent = readLimit;
      return readLimit;
    }
  }
  return m_batteryLimitPercent;
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
