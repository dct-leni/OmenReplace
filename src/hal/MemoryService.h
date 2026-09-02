#pragma once
#include <windows.h>

class MemoryService {
public:
  static MemoryService &Get();
  void Optimize(); // Working-set memory cleanup

private:
  MemoryService() = default;
};
