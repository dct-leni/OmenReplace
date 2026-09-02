#include "FanController.h"
#include "OmenEc.h"
#include "OmenLog.h"
#include "PowerControl.h"

FanController &FanController::Get() {
  static FanController instance;
  return instance;
}

FanController::FanController() {}

bool FanController::AssertTargets(int cpuPercent, int gpuPercent) {
  std::lock_guard<std::mutex> lock(m_mutex);
  OmenEc::Get().SetFanMode(true);
  OmenEc::Get().SetFanSpeedPercent(0, cpuPercent);
  OmenEc::Get().SetFanSpeedPercent(1, gpuPercent);

  if (m_wmiDisabled) {
    static int s_retryCounter = 0;
    if (++s_retryCounter > 10) {
      s_retryCounter = 0;
      m_wmiDisabled = false;
      m_wmiFailCount = 0;
    } else {
      OmenLog("[AMDOMEN] fan_hw wmi=skipped ec=written cpu=%d gpu=%d\n",
              cpuPercent, gpuPercent);
      return true;
    }
  }

  bool ok = PowerControl::Get().SetFanLevelWmiBg(cpuPercent, gpuPercent);
  if (!ok) {
    ++m_wmiFailCount;
    if (m_wmiFailCount >= 5) {
      m_wmiDisabled = true;
      OmenLog("[AMDOMEN] fan_hw wmi_disabled after %d consecutive failures; continuing EC-only\n",
              m_wmiFailCount);
    }
  } else {
    m_wmiFailCount = 0;
  }
  return ok;
}

void FanController::ReassertEc(int cpuPercent, int gpuPercent) {
  std::lock_guard<std::mutex> lock(m_mutex);
  OmenEc::Get().SetFanMode(true);
  OmenEc::Get().SetFanSpeedPercent(0, cpuPercent);
  OmenEc::Get().SetFanSpeedPercent(1, gpuPercent);
}

void FanController::Heartbeat() {
  std::lock_guard<std::mutex> lock(m_mutex);
  OmenEc::Get().FanHeartbeat();
}

void FanController::RestoreBios() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_wmiDisabled = false;
  m_wmiFailCount = 0;
  OmenEc::Get().RestoreAutoControl();
  PowerControl::Get().RestoreFanAuto();
}
