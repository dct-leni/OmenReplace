#include "MemoryService.h"
#include <psapi.h>
#include <tlhelp32.h>

MemoryService &MemoryService::Get() {
  static MemoryService instance;
  return instance;
}

void MemoryService::Optimize() {
  HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hProcessSnap == INVALID_HANDLE_VALUE)
    return;

  PROCESSENTRY32 pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32);

  if (Process32First(hProcessSnap, &pe32)) {
    do {
      HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA,
                                    FALSE, pe32.th32ProcessID);
      if (hProcess) {
        EmptyWorkingSet(hProcess);
        CloseHandle(hProcess);
      }
    } while (Process32Next(hProcessSnap, &pe32));
  }
  CloseHandle(hProcessSnap);
  EmptyWorkingSet(GetCurrentProcess());
}
