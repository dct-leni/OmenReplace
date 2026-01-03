#include "WmiHelper.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <tlhelp32.h>
#include <vector>

WmiHelper::WmiHelper() {}

WmiHelper::~WmiHelper() { Cleanup(); }

bool WmiHelper::Initialize(const std::wstring &namespc) {
  if (m_initialized)
    return true;

  HRESULT hres;

  // Step 1: Initialize COM. (Caller thread should do this usually, but we
  // assume it's done or we do it) We do NOT call CoInitialize here as it
  // depends on threading model of caller.

  // Step 2: Set general COM security levels
  hres =
      CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                           RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
  // Ignore RPC_E_TOO_LATE

  // Step 3: Obtain the initial locator to WMI
  hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (LPVOID *)&m_pLoc);

  if (FAILED(hres))
    return false;

  // Step 4: Connect to WMI
  BSTR path = SysAllocString(namespc.c_str());
  hres = m_pLoc->ConnectServer(path, NULL, NULL, NULL, 0, NULL, NULL, &m_pSvc);
  SysFreeString(path);

  if (FAILED(hres)) {
    m_pLoc->Release();
    m_pLoc = nullptr;
    return false;
  }

  // Step 5: Set security levels on the proxy
  hres = CoSetProxyBlanket(m_pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           NULL, EOAC_NONE);

  if (FAILED(hres)) {
    m_pSvc->Release();
    m_pLoc->Release();
    return false;
  }

  m_initialized = true;
  return true;
}

void WmiHelper::Cleanup() {
  if (m_pSvc) {
    m_pSvc->Release();
    m_pSvc = nullptr;
  }
  if (m_pLoc) {
    m_pLoc->Release();
    m_pLoc = nullptr;
  }
  m_initialized = false;
}

bool WmiHelper::ExecQuery(const std::wstring &query,
                          const std::wstring &propertyName,
                          std::variant<std::wstring, int, bool> &outValue) {
  if (!m_initialized)
    return false;

  IEnumWbemClassObject *pEnumerator = NULL;
  BSTR lang = SysAllocString(L"WQL");
  BSTR q = SysAllocString(query.c_str());

  HRESULT hres = m_pSvc->ExecQuery(
      lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL,
      &pEnumerator);

  SysFreeString(lang);
  SysFreeString(q);

  if (FAILED(hres))
    return false;

  IWbemClassObject *pclsObj = NULL;
  ULONG uReturn = 0;
  bool found = false;
  while (pEnumerator) {
    HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
    if (0 == uReturn)
      break;

    VARIANT vtProp;
    if (SUCCEEDED(pclsObj->Get(propertyName.c_str(), 0, &vtProp, 0, 0))) {
      if (vtProp.vt == VT_I4) {
        outValue = (int)vtProp.intVal;
        found = true;
      } else if (vtProp.vt == VT_BSTR) {
        outValue = std::wstring(vtProp.bstrVal);
        found = true;
      } else if (vtProp.vt == VT_BOOL) {
        outValue = (bool)(vtProp.boolVal != VARIANT_FALSE);
        found = true;
      }
      VariantClear(&vtProp);
    }
    pclsObj->Release();
    if (found)
      break;
  }
  pEnumerator->Release();
  return found;
}

bool WmiHelper::ExecQueryAll(const std::wstring &query,
                             const std::wstring &propertyName,
                             std::vector<int> &outValues) {
  if (!m_initialized)
    return false;

  IEnumWbemClassObject *pEnumerator = NULL;
  BSTR lang = SysAllocString(L"WQL");
  BSTR q = SysAllocString(query.c_str());

  HRESULT hres = m_pSvc->ExecQuery(
      lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL,
      &pEnumerator);

  SysFreeString(lang);
  SysFreeString(q);

  if (FAILED(hres))
    return false;

  IWbemClassObject *pclsObj = NULL;
  ULONG uReturn = 0;
  while (pEnumerator) {
    HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
    if (0 == uReturn)
      break;

    VARIANT vtProp;
    if (SUCCEEDED(pclsObj->Get(propertyName.c_str(), 0, &vtProp, 0, 0))) {
      if (vtProp.vt == VT_I4) {
        outValues.push_back((int)vtProp.intVal);
      }
      VariantClear(&vtProp);
    }
    pclsObj->Release();
  }
  pEnumerator->Release();
  return true;
}

static float GetFloatState(IWbemClassObject *pclsObj,
                           const std::wstring &prop) {
  VARIANT vtProp;
  VariantInit(&vtProp);
  if (SUCCEEDED(pclsObj->Get(prop.c_str(), 0, &vtProp, 0, 0))) {
    float ret = 0.0f;
    if (vtProp.vt == VT_R4)
      ret = vtProp.fltVal;
    else if (vtProp.vt == VT_BSTR && vtProp.bstrVal)
      ret = (float)_wtof(vtProp.bstrVal);
    VariantClear(&vtProp);
    return ret;
  }
  return 0.0f;
}

static std::wstring GetStrState(IWbemClassObject *pclsObj,
                                const std::wstring &prop) {
  VARIANT vtProp;
  VariantInit(&vtProp);
  if (SUCCEEDED(pclsObj->Get(prop.c_str(), 0, &vtProp, 0, 0))) {
    std::wstring ret = L"";
    if (vtProp.vt == VT_BSTR && vtProp.bstrVal)
      ret = vtProp.bstrVal;
    VariantClear(&vtProp);
    return ret;
  }
  return L"";
}

bool WmiHelper::QueryLhmSensors(std::vector<LhmSensorResult> &outSensors) {
  if (!m_initialized || !m_pSvc)
    return false;

  IEnumWbemClassObject *pEnumerator = NULL;
  BSTR lang = SysAllocString(L"WQL");
  BSTR query =
      SysAllocString(L"SELECT Name, Identifier, SensorType, Value FROM Sensor");

  HRESULT hres = m_pSvc->ExecQuery(
      lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL,
      &pEnumerator);

  SysFreeString(lang);
  SysFreeString(query);

  if (FAILED(hres))
    return false;

  IWbemClassObject *pclsObj = NULL;
  ULONG uReturn = 0;

  while (pEnumerator) {
    hres = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
    if (0 == uReturn)
      break;

    LhmSensorResult obj;
    obj.Name = GetStrState(pclsObj, L"Name");
    obj.Identifier = GetStrState(pclsObj, L"Identifier");
    obj.SensorType = GetStrState(pclsObj, L"SensorType");
    obj.Value = GetFloatState(pclsObj, L"Value");

    outSensors.push_back(obj);

    pclsObj->Release();
  }
  pEnumerator->Release();
  return true;
}

bool WmiHelper::IsDesktopMode() {
  std::variant<std::wstring, int, bool> val;
  if (ExecQuery(L"SELECT PCSystemType FROM Win32_ComputerSystem",
                L"PCSystemType", val)) {
    if (std::holds_alternative<int>(val)) {
      int type = std::get<int>(val);
      if (type == 1)
        return true; // Desktop
    }
  }
  return false;
}

bool WmiHelper::IsAnotherFanControllerActive() {
  bool active = false;
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(hSnapshot, &pe32)) {
      do {
        std::wstring name = pe32.szExeFile;
        if (name == L"OmenCommandCenterBg.exe" ||
            name == L"OMEN Gaming Hub.exe" ||
            name == L"OMENCommandCenter.exe" || name == L"OmenMon.exe") {
          active = true;
          break;
        }
      } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
  }
  return active;
}

bool WmiHelper::ExecuteHpBiosMethod(uint32_t command, uint32_t commandType,
                                    uint8_t *data, size_t dataSize,
                                    std::vector<uint8_t> &outData,
                                    size_t expectedOutSize) {
  if (!m_initialized || !m_pSvc) {
    return false;
  }

  HRESULT hr;
  IWbemClassObject *pClass = NULL;
  IWbemClassObject *pInParamsDefinition = NULL;
  IWbemClassObject *pInInstance = NULL;
  IWbemClassObject *pDataInClass = NULL;
  IWbemClassObject *pDataInInstance = NULL;

  BSTR className = SysAllocString(L"hpqBIntM");
  // Construct method name: hpqBIOSInt0, hpqBIOSInt4, hpqBIOSInt128
  // WE MUST always append the size, even if 0.
  std::wstring methodStr = L"hpqBIOSInt" + std::to_wstring(expectedOutSize);

  BSTR methodName = SysAllocString(methodStr.c_str());
  BSTR dataInClassName = SysAllocString(L"hpqBDataIn");

  // 1. Get method definition
  hr = m_pSvc->GetObject(className, 0, NULL, &pClass, NULL);
  if (FAILED(hr)) {
    goto cleanup;
  }

  hr = pClass->GetMethod(methodName, 0, &pInParamsDefinition, NULL);
  if (FAILED(hr)) {
    goto cleanup;
  }

  hr = pInParamsDefinition->SpawnInstance(0, &pInInstance);
  if (FAILED(hr)) {
    goto cleanup;
  }

  // 2. Create the input data object (hpqBDataIn)
  hr = m_pSvc->GetObject(dataInClassName, 0, NULL, &pDataInClass, NULL);
  if (FAILED(hr)) {
    goto cleanup;
  }

  hr = pDataInClass->SpawnInstance(0, &pDataInInstance);
  if (FAILED(hr)) {
    goto cleanup;
  }

  // Fill the hpqBDataIn instance
  {
    VARIANT vt;
    vt.vt = VT_I4;

    vt.lVal = (int32_t)command;
    pDataInInstance->Put(L"Command", 0, &vt, 0);

    vt.lVal = (int32_t)commandType;
    pDataInInstance->Put(L"CommandType", 0, &vt, 0);

    vt.lVal = (int32_t)dataSize;
    pDataInInstance->Put(L"Size", 0, &vt, 0);

    // Sign "SECU"
    SAFEARRAYBOUND signBound[1];
    signBound[0].lLbound = 0;
    signBound[0].cElements = 4;
    SAFEARRAY *saSign = SafeArrayCreate(VT_UI1, 1, signBound);
    uint8_t *pSign;
    SafeArrayAccessData(saSign, (void **)&pSign);
    pSign[0] = 0x53;
    pSign[1] = 0x45;
    pSign[2] = 0x43;
    pSign[3] = 0x55;
    SafeArrayUnaccessData(saSign);
    vt.vt = VT_ARRAY | VT_UI1;
    vt.parray = saSign;
    pDataInInstance->Put(L"Sign", 0, &vt, 0);
    VariantClear(&vt);

    // Data
    if (data && dataSize > 0) {
      SAFEARRAYBOUND bound[1];
      bound[0].lLbound = 0;
      bound[0].cElements = (ULONG)dataSize;
      SAFEARRAY *saData = SafeArrayCreate(VT_UI1, 1, bound);
      void *pBits;
      SafeArrayAccessData(saData, &pBits);
      memcpy(pBits, data, dataSize);
      SafeArrayUnaccessData(saData);
      vt.vt = VT_ARRAY | VT_UI1;
      vt.parray = saData;
      pDataInInstance->Put(L"hpqBData", 0, &vt, 0);
      VariantClear(&vt);
    }
  }

  // 3. Set the InData parameter
  {
    VARIANT vt;
    vt.vt = VT_UNKNOWN;
    vt.punkVal = pDataInInstance;
    pInInstance->Put(L"InData", 0, &vt, 0);
  }

  // 4. Execute
  {
    IWbemClassObject *pOutParams = NULL;
    BSTR instPath =
        SysAllocString(L"hpqBIntM.InstanceName='ACPI\\PNP0C14\\0_0'");
    hr = m_pSvc->ExecMethod(instPath, methodName, 0, NULL, pInInstance,
                            &pOutParams, NULL);
    SysFreeString(instPath);

    if (FAILED(hr)) {
      // WMI method call itself failed
    } else if (pOutParams) {
      VARIANT vtOut;
      VariantInit(&vtOut);
      if (SUCCEEDED(pOutParams->Get(L"OutData", 0, &vtOut, NULL, NULL))) {
        if (vtOut.vt == VT_UNKNOWN || vtOut.vt == VT_DISPATCH) {
          IUnknown *pUnk = vtOut.punkVal;
          IWbemClassObject *pOutDataObj = NULL;
          if (SUCCEEDED(pUnk->QueryInterface(IID_IWbemClassObject,
                                             (void **)&pOutDataObj))) {
            // CRITICAL: Check rwReturnCode
            VARIANT vtRetCode;
            VariantInit(&vtRetCode);
            int returnCode = -1;
            if (SUCCEEDED(pOutDataObj->Get(L"rwReturnCode", 0, &vtRetCode, NULL,
                                           NULL))) {
              if (vtRetCode.vt == VT_I4)
                returnCode = vtRetCode.lVal;
              else if (vtRetCode.vt == VT_UI4)
                returnCode = (int)vtRetCode.uintVal;
              VariantClear(&vtRetCode);
            }

            if (returnCode == 0) {
              // Only read Data if return code is OK
              VARIANT vtArray;
              VariantInit(&vtArray);
              if (SUCCEEDED(
                      pOutDataObj->Get(L"Data", 0, &vtArray, NULL, NULL))) {
                if (vtArray.vt == (VT_ARRAY | VT_UI1)) {
                  long lBound, uBound;
                  SafeArrayGetLBound(vtArray.parray, 1, &lBound);
                  SafeArrayGetUBound(vtArray.parray, 1, &uBound);
                  long len = uBound - lBound + 1;
                  uint8_t *pRaw;
                  SafeArrayAccessData(vtArray.parray, (void **)&pRaw);
                  outData.assign(pRaw, pRaw + len);
                  SafeArrayUnaccessData(vtArray.parray);
                }
                VariantClear(&vtArray);
              }
            } else {
              // BIOS returned error — treat entire call as failure
              hr = E_FAIL;
            }
            pOutDataObj->Release();
          }
        }
        VariantClear(&vtOut);
      }
      pOutParams->Release();
    }
  }

cleanup:
  SysFreeString(className);
  SysFreeString(methodName);
  SysFreeString(dataInClassName);
  if (pClass)
    pClass->Release();
  if (pInParamsDefinition)
    pInParamsDefinition->Release();
  if (pInInstance)
    pInInstance->Release();
  if (pDataInClass)
    pDataInClass->Release();
  if (pDataInInstance)
    pDataInInstance->Release();

  return SUCCEEDED(hr);
}
