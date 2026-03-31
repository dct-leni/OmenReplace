#pragma once
#include <windows.h>

class MemoryService {
public:
  static MemoryService &Get();
  void Optimize();

private:
  MemoryService() = default;
};
