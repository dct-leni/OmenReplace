#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>


// PawnIO Library wrapper
class PawnIO {
public:
  PawnIO();
  ~PawnIO();

  // Initialize library (static needed for global DLL load)
  static bool InitLibrary();

  // Load a module
  bool Load(const std::string &modulePath);
  bool LoadBuffer(const unsigned char *buffer, size_t size);

  // Execute a function
  bool Execute(const std::string &functionName,
               const std::vector<uint64_t> &inputs,
               std::vector<uint64_t> &outputs);

  bool IsLoaded() const { return m_handle != NULL; }

private:
  HANDLE m_handle = NULL;

  // Function pointers
  static HMODULE s_hLib;
  static void *s_pfnOpen;
  static void *s_pfnLoad;
  static void *s_pfnExecute;
  static void *s_pfnClose;
};
