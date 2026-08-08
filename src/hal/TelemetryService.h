#pragma once
#include <atomic>
#include <mutex>
#include <string>

// Enable with compile flag: -DOMEN_TELEMETRY or #define below
// #define OMEN_TELEMETRY

class TelemetryService {
public:
  static TelemetryService &Get();

  void Update(float cpuTemp, float gpuTemp, float fan1Rpm, float fan2Rpm,
              float cpuLoad, float gpuLoad, float totalPower);

private:
  TelemetryService();
  ~TelemetryService();

  std::mutex m_mutex;
  std::atomic<bool> m_enabled{false};
  uint64_t m_lastWriteMs = 0;
  int m_writeCount = 0;
};
