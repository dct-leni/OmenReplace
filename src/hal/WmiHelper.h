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

  // HP Omen Specific
  bool ExecuteHpBiosMethod(uint32_t command, uint32_t commandType,
                           uint8_t *data, size_t dataSize,
                           std::vector<uint8_t> &outData,
                           size_t expectedOutSize = 0);

  // Desktop detection
  bool IsDesktopMode();

private:
  bool m_initialized = false;
  IWbemLocator *m_pLoc = nullptr;
  IWbemServices *m_pSvc = nullptr;
};
