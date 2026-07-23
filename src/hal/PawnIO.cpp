#include "PawnIO.h"
#include <fstream>
#include <iostream>
#include <vector>

// Static members
HMODULE PawnIO::s_hLib = NULL;
void *PawnIO::s_pfnOpen = nullptr;
void *PawnIO::s_pfnLoad = nullptr;
void *PawnIO::s_pfnExecute = nullptr;
void *PawnIO::s_pfnClose = nullptr;

typedef HRESULT(STDAPICALLTYPE *FnOpen)(PHANDLE);
typedef HRESULT(STDAPICALLTYPE *FnLoad)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT(STDAPICALLTYPE *FnExecute)(HANDLE, PCSTR, const ULONG64 *,
                                           SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT(STDAPICALLTYPE *FnClose)(HANDLE);

void LogPawnIO(const std::string &msg) {}

bool PawnIO::InitLibrary() {
  if (s_hLib)
    return true;

  LogPawnIO("=== PawnIO Library Initialization ===");

  s_hLib = LoadLibraryA("PawnIOLib.dll");
  if (!s_hLib) {
    s_hLib = LoadLibraryA("C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!s_hLib) {
      LogPawnIO("Failed to load PawnIOLib.dll");
      // Try explicit loading of PawnIOLib if in bin too?
      return false;
    }
  }

  s_pfnOpen = (void *)GetProcAddress(s_hLib, "pawnio_open");
  s_pfnLoad = (void *)GetProcAddress(s_hLib, "pawnio_load");
  s_pfnExecute = (void *)GetProcAddress(s_hLib, "pawnio_execute");
  s_pfnClose = (void *)GetProcAddress(s_hLib, "pawnio_close");

  if (!s_pfnOpen || !s_pfnLoad || !s_pfnExecute || !s_pfnClose) {
    LogPawnIO("Failed to get PawnIO function pointers");
    return false;
  }
  return true;
}

PawnIO::PawnIO() {
  if (!s_hLib)
    InitLibrary();
  if (s_hLib) {
    ((FnOpen)s_pfnOpen)(&m_handle);
  }
}

PawnIO::~PawnIO() {
  if (m_handle && s_pfnClose) {
    ((FnClose)s_pfnClose)(m_handle);
  }
}

bool PawnIO::Load(const std::string &modulePath) {
  if (!m_handle)
    return false;

  // Safe path resolving relative to EXE
  char path[MAX_PATH];
  if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0)
    return false;

  std::string fullPath;
  if (modulePath.find(':') != std::string::npos ||
      modulePath.find("\\\\") == 0) {
    fullPath = modulePath;
  } else {
    std::string exePathStr = path;
    size_t lastSlash = exePathStr.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
      fullPath = exePathStr.substr(0, lastSlash) + "\\" + modulePath;
    } else {
      fullPath = modulePath;
    }
  }

  std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    LogPawnIO("Failed to open module file: " + fullPath);
    return false;
  }

  size_t size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(size);
  file.read((char *)buffer.data(), size);

  HRESULT hr = ((FnLoad)s_pfnLoad)(m_handle, buffer.data(), size);
  if (FAILED(hr)) {
    char msg[256];
    sprintf(msg, "pawnio_load failed for %s: 0x%08lX", modulePath.c_str(), hr);
    LogPawnIO(msg);
    return false;
  }

  LogPawnIO("Module loaded: " + fullPath);
  return true;
}

bool PawnIO::LoadBuffer(const unsigned char *buffer, size_t size) {
  if (!m_handle || !buffer || size == 0)
    return false;

  HRESULT hr =
      ((FnLoad)s_pfnLoad)(m_handle, (const UCHAR *)buffer, (SIZE_T)size);
  if (FAILED(hr)) {
    char msg[256];
    sprintf(msg, "pawnio_load buffer failed: 0x%08lX", hr);
    LogPawnIO(msg);
    return false;
  }

  LogPawnIO("Module loaded from buffer");
  return true;
}

bool PawnIO::Execute(const std::string &functionName,
                     const std::vector<uint64_t> &inputs,
                     std::vector<uint64_t> &outputs) {
  if (!m_handle)
    return false;

  SIZE_T returnSize = 0;
  HRESULT hr = ((FnExecute)s_pfnExecute)(
      m_handle, functionName.c_str(), inputs.empty() ? nullptr : inputs.data(),
      inputs.size(), outputs.empty() ? nullptr : outputs.data(), outputs.size(),
      &returnSize);

  if (FAILED(hr)) {
    return false;
  }

  outputs.resize(returnSize);
  return true;
}
