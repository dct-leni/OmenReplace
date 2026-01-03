#pragma once
#include <windows.h>
#include <cstring>
#include <iostream>
#include <vector>

// ── Minimal NVAPI Types ──────────────────────────────────────────────────────
typedef int NvAPI_Status;
#define NVAPI_OK 0

typedef void *NvPhysicalGpuHandle;
#define NVAPI_MAX_PHYSICAL_GPUS 64
#define NVAPI_MAX_THERMAL_SENSORS_PER_GPU 3

// Thermal settings
typedef struct {
  unsigned int version;
  unsigned int count;
  struct {
    int controller;
    int target;
    int currentTemp;
  } sensor[NVAPI_MAX_THERMAL_SENSORS_PER_GPU];
} NV_GPU_THERMAL_SETTINGS;
#define NV_GPU_THERMAL_SETTINGS_VER_1 (sizeof(NV_GPU_THERMAL_SETTINGS) | (1 << 16))
#define NVAPI_THERMAL_TARGET_GPU 1

// ── Clock offset structures (NvAPI_GPU_GetClockBoostMask / ClockFreqTable) ──
// We use the undocumented pstate offset approach (standard in MSI Afterburner)
#define NV_GPU_PERF_PSTATES20_MAX_PSTATES 16
#define NV_GPU_PERF_PSTATES20_MAX_CLOCKS  8
#define NV_GPU_PERF_PSTATES20_MAX_BASE_VOLTAGES 4
typedef unsigned int NvU32;
typedef int NvS32;

typedef enum {
  NVAPI_GPU_PERF_PSTATE_P0 = 0,
} NV_GPU_PERF_PSTATE_ID;

typedef enum {
  NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS = 0,
  NVAPI_GPU_PUBLIC_CLOCK_MEMORY = 4,
} NV_GPU_PUBLIC_CLOCK_ID;

typedef struct {
  NvU32 domainId;
  NvU32 typeId;
  NvU32 bIsEditable : 1;
  NvU32 reserved : 31;
  union {
    struct {
      NvU32 freq_kHz;
    } single;
  } data;
  NvS32 freqDelta_kHz; // clock offset — negative=downclock, positive=overclock
} NV_GPU_PSTATE20_CLOCK_ENTRY_V1;

typedef struct {
  NvU32 version;
  NvU32 bIsEditable : 1;
  NvU32 reserved : 31;
  NvU32 numPstates;
  NvU32 numClocks;
  NvU32 numBaseVoltages;
  struct {
    NV_GPU_PERF_PSTATE_ID pstateId;
    NvU32 bIsEditable : 1;
    NvU32 reserved : 31;
    NV_GPU_PSTATE20_CLOCK_ENTRY_V1 clocks[NV_GPU_PERF_PSTATES20_MAX_CLOCKS];
    struct {
      NvU32 domainId;
      NvU32 bIsEditable : 1;
      NvU32 reserved : 31;
      NvU32 volt_uV;
      NvS32 voltDelta_uV;
    } baseVoltages[NV_GPU_PERF_PSTATES20_MAX_BASE_VOLTAGES];
  } pstates[NV_GPU_PERF_PSTATES20_MAX_PSTATES];
} NV_GPU_PERF_PSTATES20_INFO_V1;

#define NV_GPU_PERF_PSTATES20_INFO_VER1 (sizeof(NV_GPU_PERF_PSTATES20_INFO_V1) | (1 << 16))
#define NV_GPU_PERF_PSTATES20_INFO_VER  NV_GPU_PERF_PSTATES20_INFO_VER1

// ── Power limit structures ────────────────────────────────────────────────────
#define NVAPI_MAX_GPU_PERF_PSTATES 16
typedef struct {
  NvU32 version;
  NvU32 bIsEditable : 1;
  NvU32 reserved    : 31;
  NvU32 numPowerChannels;
  struct {
    NvU32 controlId;
    NvU32 bIsEditable : 1;
    NvU32 reserved    : 31;
    NvU32 powerBudget_mW; // current power limit in mW
    NvU32 defaultPowerBudget_mW;
    NvU32 minPowerBudget_mW;
    NvU32 maxPowerBudget_mW;
  } entries[4];
} NV_GPU_POWER_STATUS;
#define NV_GPU_POWER_STATUS_VER (sizeof(NV_GPU_POWER_STATUS) | (1 << 16))

// Function pointer types
typedef void *(*NvAPI_QueryInterface_t)(unsigned int);
typedef NvAPI_Status (*NvAPI_Initialize_t)();
typedef NvAPI_Status (*NvAPI_EnumPhysicalGPUs_t)(NvPhysicalGpuHandle[NVAPI_MAX_PHYSICAL_GPUS], unsigned long *);
typedef NvAPI_Status (*NvAPI_GPU_GetThermalSettings_t)(NvPhysicalGpuHandle, unsigned long, NV_GPU_THERMAL_SETTINGS *);
typedef NvAPI_Status (*NvAPI_GPU_GetPstates20_t)(NvPhysicalGpuHandle, NV_GPU_PERF_PSTATES20_INFO_V1 *);
typedef NvAPI_Status (*NvAPI_GPU_SetPstates20_t)(NvPhysicalGpuHandle, NV_GPU_PERF_PSTATES20_INFO_V1 *);
typedef NvAPI_Status (*NvAPI_DLL_ClientPowerPoliciesGetStatus_t)(NvPhysicalGpuHandle, NV_GPU_POWER_STATUS *);
typedef NvAPI_Status (*NvAPI_DLL_ClientPowerPoliciesSetStatus_t)(NvPhysicalGpuHandle, NV_GPU_POWER_STATUS *);

// ── NVAPI magic offsets (well-known, used by MSI Afterburner, GPU-Z, etc.) ──
// These are stable across driver versions
#define NVAPI_ID_INIT               0x0150E828
#define NVAPI_ID_ENUMGPUS           0xE5AC921F
#define NVAPI_ID_THERMALSETTINGS    0xE3640A56
#define NVAPI_ID_GETPSTATES20       0x6FF81213
#define NVAPI_ID_SETPSTATES20       0x0F4DAE6B
#define NVAPI_ID_POWERPOL_GET       0x70916171
#define NVAPI_ID_POWERPOL_SET       0xAD95F5ED

// ─────────────────────────────────────────────────────────────────────────────

class NvApiHelper {
public:
  NvApiHelper() {}
  ~NvApiHelper() {
    if (m_hModule)
      FreeLibrary(m_hModule);
  }

  bool Initialize() {
    if (m_initialized)
      return true;

    m_hModule = LoadLibraryA("nvapi64.dll");
    if (!m_hModule)
      return false;

    NvAPI_QueryInterface_t qi =
        (NvAPI_QueryInterface_t)GetProcAddress(m_hModule, "nvapi_QueryInterface");
    if (!qi)
      return false;

    fn_Init             = (NvAPI_Initialize_t)                           qi(NVAPI_ID_INIT);
    fn_EnumGPUs         = (NvAPI_EnumPhysicalGPUs_t)                     qi(NVAPI_ID_ENUMGPUS);
    fn_ThermalSettings  = (NvAPI_GPU_GetThermalSettings_t)               qi(NVAPI_ID_THERMALSETTINGS);
    fn_GetPstates20     = (NvAPI_GPU_GetPstates20_t)                     qi(NVAPI_ID_GETPSTATES20);
    fn_SetPstates20     = (NvAPI_GPU_SetPstates20_t)                     qi(NVAPI_ID_SETPSTATES20);
    fn_PowerPolicyGet   = (NvAPI_DLL_ClientPowerPoliciesGetStatus_t)     qi(NVAPI_ID_POWERPOL_GET);
    fn_PowerPolicySet   = (NvAPI_DLL_ClientPowerPoliciesSetStatus_t)     qi(NVAPI_ID_POWERPOL_SET);

    if (!fn_Init || !fn_EnumGPUs)
      return false;

    if (fn_Init() != NVAPI_OK)
      return false;

    // Cache first GPU handle
    unsigned long count = 0;
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {0};
    if (fn_EnumGPUs(handles, &count) == NVAPI_OK && count > 0) {
      m_gpu = handles[0];
      m_gpuCount = (int)count;
    } else {
      return false;
    }

    m_initialized = true;
    return true;
  }

  bool IsInitialized() const { return m_initialized; }

  float GetGpuTemp() {
    if (!m_initialized || !fn_ThermalSettings)
      return 0.0f;
    NV_GPU_THERMAL_SETTINGS ts = {0};
    ts.version = NV_GPU_THERMAL_SETTINGS_VER_1;
    if (fn_ThermalSettings(m_gpu, 0, &ts) == NVAPI_OK)
      return (float)ts.sensor[0].currentTemp;
    return 0.0f;
  }

  // Returns P0 clock offsets in MHz. Returns false if unsupported.
  bool GetClockOffsets(int &coreOffsetMHz, int &memOffsetMHz) {
    if (!m_initialized || !fn_GetPstates20)
      return false;

    NV_GPU_PERF_PSTATES20_INFO_V1 info;
    memset(&info, 0, sizeof(info));
    info.version = NV_GPU_PERF_PSTATES20_INFO_VER;

    if (fn_GetPstates20(m_gpu, &info) != NVAPI_OK)
      return false;

    coreOffsetMHz = 0;
    memOffsetMHz  = 0;

    // P0 state = highest performance
    for (NvU32 p = 0; p < info.numPstates; p++) {
      if (info.pstates[p].pstateId != NVAPI_GPU_PERF_PSTATE_P0)
        continue;
      for (NvU32 c = 0; c < info.numClocks; c++) {
        auto &clk = info.pstates[p].clocks[c];
        if (clk.domainId == NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS)
          coreOffsetMHz = clk.freqDelta_kHz / 1000;
        else if (clk.domainId == NVAPI_GPU_PUBLIC_CLOCK_MEMORY)
          memOffsetMHz = clk.freqDelta_kHz / 1000;
      }
      break;
    }
    return true;
  }

  // Sets P0 clock offsets in MHz.
  bool SetClockOffsets(int coreOffsetMHz, int memOffsetMHz) {
    if (!m_initialized || !fn_GetPstates20 || !fn_SetPstates20)
      return false;

    // Read current pstate table first
    NV_GPU_PERF_PSTATES20_INFO_V1 info;
    memset(&info, 0, sizeof(info));
    info.version = NV_GPU_PERF_PSTATES20_INFO_VER;
    if (fn_GetPstates20(m_gpu, &info) != NVAPI_OK)
      return false;

    // Clamp offsets to sane range
    coreOffsetMHz = std::max(-500, std::min(500,  coreOffsetMHz));
    memOffsetMHz  = std::max(-500, std::min(1000, memOffsetMHz));

    // Modify P0 state clocks
    for (NvU32 p = 0; p < info.numPstates; p++) {
      if (info.pstates[p].pstateId != NVAPI_GPU_PERF_PSTATE_P0)
        continue;
      for (NvU32 c = 0; c < info.numClocks; c++) {
        auto &clk = info.pstates[p].clocks[c];
        if (clk.domainId == NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS)
          clk.freqDelta_kHz = coreOffsetMHz * 1000;
        else if (clk.domainId == NVAPI_GPU_PUBLIC_CLOCK_MEMORY)
          clk.freqDelta_kHz = memOffsetMHz * 1000;
      }
      break;
    }

    // Write only P0  (numPstates=1 is the common Afterburner approach)
    info.numPstates = 1;
    return fn_SetPstates20(m_gpu, &info) == NVAPI_OK;
  }

  // Returns current power limit as % of default TDP (100 = stock).
  bool GetPowerLimitPercent(int &pct) {
    if (!m_initialized || !fn_PowerPolicyGet)
      return false;
    NV_GPU_POWER_STATUS ps;
    memset(&ps, 0, sizeof(ps));
    ps.version = NV_GPU_POWER_STATUS_VER;
    if (fn_PowerPolicyGet(m_gpu, &ps) != NVAPI_OK || ps.numPowerChannels == 0)
      return false;
    float cur = (float)ps.entries[0].powerBudget_mW;
    float def = (float)ps.entries[0].defaultPowerBudget_mW;
    pct = (def > 0) ? (int)(cur / def * 100.0f + 0.5f) : 100;
    return true;
  }

  // Returns min/max power limit as percent of default TDP.
  bool GetPowerLimitRange(int &minPct, int &maxPct) {
    if (!m_initialized || !fn_PowerPolicyGet)
      return false;
    NV_GPU_POWER_STATUS ps;
    memset(&ps, 0, sizeof(ps));
    ps.version = NV_GPU_POWER_STATUS_VER;
    if (fn_PowerPolicyGet(m_gpu, &ps) != NVAPI_OK || ps.numPowerChannels == 0)
      return false;
    float def = (float)ps.entries[0].defaultPowerBudget_mW;
    minPct = (def > 0) ? (int)((float)ps.entries[0].minPowerBudget_mW / def * 100.0f + 0.5f) : 50;
    maxPct = (def > 0) ? (int)((float)ps.entries[0].maxPowerBudget_mW / def * 100.0f + 0.5f) : 150;
    return true;
  }

  // Sets power limit as % of default TDP.
  bool SetPowerLimitPercent(int pct) {
    if (!m_initialized || !fn_PowerPolicyGet || !fn_PowerPolicySet)
      return false;
    NV_GPU_POWER_STATUS ps;
    memset(&ps, 0, sizeof(ps));
    ps.version = NV_GPU_POWER_STATUS_VER;
    if (fn_PowerPolicyGet(m_gpu, &ps) != NVAPI_OK || ps.numPowerChannels == 0)
      return false;
    float def = (float)ps.entries[0].defaultPowerBudget_mW;
    // Only set the first channel, keep others as-is
    NvU32 minW = ps.entries[0].minPowerBudget_mW;
    NvU32 maxW = ps.entries[0].maxPowerBudget_mW;
    NvU32 target = (NvU32)(def * pct / 100.0f + 0.5f);
    target = std::max(minW, std::min(maxW, target));
    ps.entries[0].powerBudget_mW = target;
    return fn_PowerPolicySet(m_gpu, &ps) == NVAPI_OK;
  }

private:
  HMODULE m_hModule = nullptr;
  bool m_initialized = false;
  NvPhysicalGpuHandle m_gpu = nullptr;
  int m_gpuCount = 0;

  NvAPI_Initialize_t                          fn_Init            = nullptr;
  NvAPI_EnumPhysicalGPUs_t                    fn_EnumGPUs        = nullptr;
  NvAPI_GPU_GetThermalSettings_t              fn_ThermalSettings = nullptr;
  NvAPI_GPU_GetPstates20_t                    fn_GetPstates20    = nullptr;
  NvAPI_GPU_SetPstates20_t                    fn_SetPstates20    = nullptr;
  NvAPI_DLL_ClientPowerPoliciesGetStatus_t    fn_PowerPolicyGet  = nullptr;
  NvAPI_DLL_ClientPowerPoliciesSetStatus_t    fn_PowerPolicySet  = nullptr;
};
