#include "TelemetryService.h"

#ifdef OMEN_TELEMETRY
#include <chrono>
#include <fstream>
#include <iostream>

static std::ofstream s_telemetryFile;
static bool s_headerWritten = false;
#endif

TelemetryService &TelemetryService::Get() {
  static TelemetryService instance;
  return instance;
}

TelemetryService::TelemetryService() {
#ifdef OMEN_TELEMETRY
  m_enabled = true;
#endif
}

TelemetryService::~TelemetryService() {
#ifdef OMEN_TELEMETRY
  if (s_telemetryFile.is_open())
    s_telemetryFile.close();
#endif
}

void TelemetryService::Update(float cpuTemp, float gpuTemp, float fan1Rpm,
                               float fan2Rpm, float cpuLoad, float gpuLoad,
                               float totalPower) {
  if (!m_enabled)
    return;

#ifdef OMEN_TELEMETRY
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();

  // Write every 2 seconds
  if (now - m_lastWriteMs < 2000)
    return;
  m_lastWriteMs = now;

  std::lock_guard<std::mutex> lock(m_mutex);

  if (!s_telemetryFile.is_open()) {
    s_telemetryFile.open("telemetry.csv", std::ios::app);
    if (!s_telemetryFile.is_open())
      return;
  }

  if (!s_headerWritten) {
    s_telemetryFile
        << "timestamp_ms,cpu_temp_c,gpu_temp_c,fan1_rpm,fan2_rpm,"
           "cpu_load_pct,gpu_load_pct,total_power_w\n";
    s_headerWritten = true;
  }

  s_telemetryFile << now << "," << cpuTemp << "," << gpuTemp << "," << fan1Rpm
                  << "," << fan2Rpm << "," << cpuLoad << "," << gpuLoad << ","
                  << totalPower << "\n";

  m_writeCount++;
  // Flush every 10 writes (20 seconds)
  if (m_writeCount % 10 == 0)
    s_telemetryFile.flush();
#endif
}
