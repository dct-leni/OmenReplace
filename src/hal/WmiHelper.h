#pragma once
#include <Wbemidl.h>
#include <comdef.h>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>


#ifdef _MSC_VER
#pragma comment(lib, "wbemuuid.lib")
#endif

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

  // HP Omen Specific
  bool ExecuteHpBiosMethod(uint32_t command, uint32_t commandType,
                           uint8_t *data, size_t dataSize,
                           std::vector<uint8_t> &outData,
                           size_t expectedOutSize = 0);

  // Generic instance property access (e.g. root\standardcimv2 net adapters).
  // Returns __RELPATH of all instances of a class.
  std::vector<std::wstring> GetInstancePaths(const std::wstring &className);
  // Read a UInt32 property from one instance (by relpath).
  bool GetUint32Property(const std::wstring &relPath,
                         const std::wstring &propName, uint32_t &out);
  // Write a UInt32 property on one instance and commit (PutInstance).
  bool PutUint32Property(const std::wstring &relPath,
                         const std::wstring &propName, uint32_t value);

  // Desktop detection
  bool IsDesktopMode();
  bool IsAnotherFanControllerActive();

private:
  bool m_initialized = false;
  IWbemLocator *m_pLoc = nullptr;
  IWbemServices *m_pSvc = nullptr;
};
