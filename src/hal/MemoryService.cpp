#include "MemoryService.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>

MemoryService &MemoryService::Get() {
  static MemoryService instance;
  return instance;
}

void MemoryService::Optimize() {
  // Snapshot of all processes
  HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hProcessSnap == INVALID_HANDLE_VALUE)
    return;

  PROCESSENTRY32 pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32);

  if (Process32First(hProcessSnap, &pe32)) {
    do {
      // Open process with necessary rights to empty working set
      HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA,
                                    FALSE, pe32.th32ProcessID);
      if (hProcess) {
        // This moves as much memory as possible from the working set to the
        // standby or modified page lists.
        EmptyWorkingSet(hProcess);
        CloseHandle(hProcess);
      }
    } while (Process32Next(hProcessSnap, &pe32));
  }
  CloseHandle(hProcessSnap);

  // Also optimize current process itself
  EmptyWorkingSet(GetCurrentProcess());
}
