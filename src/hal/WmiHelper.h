#pragma once
#include <Wbemidl.h>
#include <comdef.h>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>


#pragma comment(lib, "wbemuuid.lib")

// Global struct to avoid scope issues
struct LhmSensorResult {
  std::wstring Name;
  std::wstring Identifier;
  std::wstring SensorType;
  float Value;
};

class WmiHelper {
public:
  WmiHelper();
  ~WmiHelper();

  bool Initialize(const std::wstring &namespc = L"ROOT\\WMI");
  void Cleanup();

  // Executes a WQL query (single value result)
  bool ExecQuery(const std::wstring &query, const std::wstring &propertyName,
                 std::variant<std::wstring, int, bool> &outValue);

  // Executes a WQL query (multiple integer values, e.g. thermal zones)
  bool ExecQueryAll(const std::wstring &query, const std::wstring &propertyName,
                    std::vector<int> &outValues);

  // Specific query for LibreHardwareMonitor sensors
  bool QueryLhmSensors(std::vector<LhmSensorResult> &outSensors);

  bool ExecuteMethod(const std::wstring &className,
                     const std::wstring &methodName,
                     const std::wstring &paramName, int paramValue);

  // HP Omen Specific
  bool ExecuteHpBiosMethod(uint32_t command, uint32_t commandType,
                           uint8_t *data, size_t dataSize,
                           std::vector<uint8_t> &outData,
                           size_t expectedOutSize = 0);

  // Desktop detection
  bool IsDesktopMode();
  bool IsAnotherFanControllerActive();

private:
  bool m_initialized = false;
  IWbemLocator *m_pLoc = nullptr;
  IWbemServices *m_pSvc = nullptr;
};
