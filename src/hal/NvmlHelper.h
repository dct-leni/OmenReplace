#pragma once
#include <windows.h>


// NVML Types
typedef struct nvmlDevice_st *nvmlDevice_t;
typedef int nvmlReturn_t;

typedef struct nvmlUtilization_st {
  unsigned int gpu;
  unsigned int memory;
} nvmlUtilization_t;

#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0

// Function Pointers
typedef nvmlReturn_t (*nvmlInit_t)();
typedef nvmlReturn_t (*nvmlShutdown_t)();
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int index,
                                                     nvmlDevice_t *device);
typedef nvmlReturn_t (*nvmlDeviceGetTemperature_t)(nvmlDevice_t device,
                                                   int sensorType,
                                                   unsigned int *temp);
typedef nvmlReturn_t (*nvmlDeviceGetPowerUsage_t)(nvmlDevice_t device,
                                                  unsigned int *power);
typedef nvmlReturn_t (*nvmlDeviceGetUtilizationRates_t)(
    nvmlDevice_t device, nvmlUtilization_t *utilization);

class NvmlHelper {
public:
  NvmlHelper() {}
  ~NvmlHelper() {
    if (m_nvmlShutdown)
      m_nvmlShutdown();
    if (m_hModule)
      FreeLibrary(m_hModule);
  }

  bool Initialize() {
    if (m_initialized)
      return true;

    m_hModule = LoadLibraryA("nvml.dll");
    if (!m_hModule) {
      m_hModule = LoadLibraryA(
          "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }

    if (!m_hModule)
      return false;

    nvmlInit_t nvmlInit = (nvmlInit_t)GetProcAddress(m_hModule, "nvmlInit");
    m_nvmlShutdown = (nvmlShutdown_t)GetProcAddress(m_hModule, "nvmlShutdown");
    m_nvmlDeviceGetHandleByIndex = (nvmlDeviceGetHandleByIndex_t)GetProcAddress(
        m_hModule, "nvmlDeviceGetHandleByIndex");
    m_nvmlDeviceGetTemperature = (nvmlDeviceGetTemperature_t)GetProcAddress(
        m_hModule, "nvmlDeviceGetTemperature");
    m_nvmlDeviceGetPowerUsage = (nvmlDeviceGetPowerUsage_t)GetProcAddress(
        m_hModule, "nvmlDeviceGetPowerUsage");
    m_nvmlDeviceGetUtilizationRates =
        (nvmlDeviceGetUtilizationRates_t)GetProcAddress(
            m_hModule, "nvmlDeviceGetUtilizationRates");

    if (!nvmlInit || !m_nvmlDeviceGetHandleByIndex ||
        !m_nvmlDeviceGetTemperature)
      return false;
    if (nvmlInit() != NVML_SUCCESS)
      return false;
    if (m_nvmlDeviceGetHandleByIndex(0, &m_device) != NVML_SUCCESS)
      return false;

    m_initialized = true;
    return true;
  }

  float GetGpuTemp() {
    if (!m_initialized)
      return 0.0f;
    unsigned int temp = 0;
    if (m_nvmlDeviceGetTemperature(m_device, NVML_TEMPERATURE_GPU, &temp) ==
        NVML_SUCCESS) {
      return (float)temp;
    }
    return 0.0f;
  }

  float GetGpuPower() {
    if (!m_initialized || !m_nvmlDeviceGetPowerUsage)
      return 0.0f;
    unsigned int power = 0;
    if (m_nvmlDeviceGetPowerUsage(m_device, &power) == NVML_SUCCESS) {
      return (float)power / 1000.0f; // mW to Watts
    }
    return 0.0f;
  }

  float GetGpuLoad() {
    if (!m_initialized || !m_nvmlDeviceGetUtilizationRates)
      return 0.0f;
    nvmlUtilization_t util;
    if (m_nvmlDeviceGetUtilizationRates(m_device, &util) == NVML_SUCCESS) {
      return (float)util.gpu;
    }
    return 0.0f;
  }

  bool IsInitialized() const { return m_initialized; }

private:
  HMODULE m_hModule = nullptr;
  bool m_initialized = false;
  nvmlDevice_t m_device = nullptr;

  nvmlShutdown_t m_nvmlShutdown = nullptr;
  nvmlDeviceGetHandleByIndex_t m_nvmlDeviceGetHandleByIndex = nullptr;
  nvmlDeviceGetTemperature_t m_nvmlDeviceGetTemperature = nullptr;
  nvmlDeviceGetPowerUsage_t m_nvmlDeviceGetPowerUsage = nullptr;
  nvmlDeviceGetUtilizationRates_t m_nvmlDeviceGetUtilizationRates = nullptr;
};
