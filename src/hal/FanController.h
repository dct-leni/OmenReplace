#pragma once
#include <mutex>

// Single owner of all fan hardware writes (EC registers + WMI fan level).
// Worker, UI, and shutdown paths must route through this class so fan I/O is
// serialized by one mutex and cannot race.
class FanController {
public:
  static FanController &Get();

  // Full re-assert: EC registers + WMI fan level. Returns WMI success.
  bool AssertTargets(int cpuPercent, int gpuPercent);
  // EC-only re-enforce without WMI overhead.
  void ReassertEc(int cpuPercent, int gpuPercent);
  // EC watchdog heartbeat.
  void Heartbeat();
  // Return fans to BIOS control (EC + WMI restore).
  void RestoreBios();

 private:
  FanController();
  std::mutex m_mutex;
  int m_wmiFailCount = 0;
  bool m_wmiDisabled = false;
};
