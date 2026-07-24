#include "FanService.h"
#include "OmenEc.h"
#include "PowerControl.h"
#include "ThermalService.h"
#include <algorithm>
#include <chrono>

FanService &FanService::Get() {
  static FanService instance;
  return instance;
}

#include <fstream>

FanService::FanService() {
  // Default Curve Points (Max 90C as requested)
  static const int temps[5] = {40, 50, 65, 80, 90};
  static const int cpuS[5] = {20, 40, 60, 80, 100};
  static const int gpuS[5] = {10, 30, 50, 70, 90}; // 10% less

  for (int i = 0; i < 5; i++) {
    m_cpuCurve[i] = {temps[i], cpuS[i]};
    m_gpuCurve[i] = {temps[i], gpuS[i]};
  }

  LoadConfig();

  // Transition old configs (100C) to 90C limit
  for (int i = 0; i < 5; i++) {
    if (m_cpuCurve[i].temp > 90)
      m_cpuCurve[i].temp = 90;
    if (m_gpuCurve[i].temp > 90)
      m_gpuCurve[i].temp = 90;

    // Ensure monotonicity after clamping
    if (i > 0) {
      if (m_cpuCurve[i].temp < m_cpuCurve[i - 1].temp)
        m_cpuCurve[i].temp = m_cpuCurve[i - 1].temp;
      if (m_gpuCurve[i].temp < m_gpuCurve[i - 1].temp)
        m_gpuCurve[i].temp = m_gpuCurve[i - 1].temp;
    }
  }
}

void FanService::SaveConfig() {
  std::ofstream f("config.json");
  if (!f.is_open())
    return;
  f << "{\n";
  FanControlMode fMode = m_controlMode.load();
  int saveMode = (int)fMode;
  if (fMode == FanControlMode::Manual)
    saveMode = (int)FanControlMode::Auto;
  f << "  \"fan_mode\": " << saveMode << ",\n";
  f << "  \"power_mode\": " << (int)PowerControl::Get().GetCurrentMode()
    << ",\n";
  f << "  \"optimized_gpu_offset\": " << m_optimizedGpuOffset << ",\n";

  // Overlay & Application Settings
  f << "  \"overlay\": {\n";
  f << "    \"show\": " << (m_overlayConfig.show ? "true" : "false") << ",\n";
  f << "    \"top\": " << (m_overlayConfig.top ? "true" : "false") << ",\n";
  f << "    \"vertical\": " << (m_overlayConfig.vertical ? "true" : "false") << ",\n";
  f << "    \"opacity\": " << m_overlayConfig.opacity << ",\n";
  f << "    \"pos_x\": " << m_overlayConfig.posX << ",\n";
  f << "    \"pos_y\": " << m_overlayConfig.posY << ",\n";
  f << "    \"size_w\": " << m_overlayConfig.sizeW << ",\n";
  f << "    \"size_h\": " << m_overlayConfig.sizeH << ",\n";
  f << "    \"preset_idx\": " << m_overlayConfig.presetIdx << ",\n";
  f << "    \"cpu_ppt_cap\": " << PowerControl::Get().GetCpuPowerLimitW() << ",\n";
  f << "    \"cpu_warn\": " << m_overlayConfig.cpuWarn << ",\n";
  f << "    \"cpu_crit\": " << m_overlayConfig.cpuCrit << ",\n";
  f << "    \"gpu_warn\": " << m_overlayConfig.gpuWarn << ",\n";
  f << "    \"gpu_crit\": " << m_overlayConfig.gpuCrit << ",\n";
  f << "    \"disk_warn\": " << m_overlayConfig.diskWarn << ",\n";
  f << "    \"disk_crit\": " << m_overlayConfig.diskCrit << ",\n";
  f << "    \"battery_limit\": " << m_overlayConfig.batteryLimit << ",\n";
  f << "    \"amd_curve_optimizer\": " << PowerControl::Get().GetCachedAmdCurveOptimizer() << "\n";
  f << "  }\n";
  f << "}\n";
}

void FanService::LoadConfig() {
  std::ifstream f("config.json");
  if (!f.is_open())
    return;

  std::string line;
  bool inOverlay = false;
  while (std::getline(f, line)) {
    // Read fan_mode
    if (line.find("\"fan_mode\"") != std::string::npos) {
      size_t pos = line.find(":");
      if (pos != std::string::npos) {
        std::string valStr = line.substr(pos + 1);
        valStr.erase(
            std::remove_if(valStr.begin(), valStr.end(),
                           [](unsigned char c) { return !std::isdigit(c); }),
            valStr.end());
        if (!valStr.empty()) {
          m_controlMode = (FanControlMode)std::stoi(valStr);
        }
      }
    }
    // Read power_mode
    if (line.find("\"power_mode\"") != std::string::npos) {
      size_t pos = line.find(":");
      if (pos != std::string::npos) {
        std::string valStr = line.substr(pos + 1);
        valStr.erase(
            std::remove_if(valStr.begin(), valStr.end(),
                           [](unsigned char c) { return !std::isdigit(c); }),
            valStr.end());
        if (!valStr.empty())
          PowerControl::Get().SetMode((PowerMode)std::stoi(valStr));
      }
    }
    // Read optimized_gpu_offset
    if (line.find("\"optimized_gpu_offset\"") != std::string::npos) {
      size_t pos = line.find(":");
      if (pos != std::string::npos) {
        std::string valStr = line.substr(pos + 1);
        valStr.erase(
            std::remove_if(valStr.begin(), valStr.end(),
                           [](unsigned char c) { return !std::isdigit(c); }),
            valStr.end());
        if (!valStr.empty()) {
          int offset = std::stoi(valStr);
          m_optimizedGpuOffset = std::max(0, std::min(50, offset));
        }
      }
    }

    if (line.find("\"overlay\"") != std::string::npos) {
      inOverlay = true;
      continue;
    }

    if (inOverlay) {
      auto getVal = [&](const std::string &l) {
        size_t p = l.find(":");
        if (p == std::string::npos)
          return std::string();
        std::string v = l.substr(p + 1);
        size_t c = v.find(",");
        if (c != std::string::npos)
          v = v.substr(0, c);
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t") + 1);
        return v;
      };
      if (line.find("\"show\"") != std::string::npos)
        m_overlayConfig.show = (line.find("true") != std::string::npos);
      if (line.find("\"top\"") != std::string::npos)
        m_overlayConfig.top = (line.find("true") != std::string::npos);
      if (line.find("\"vertical\"") != std::string::npos)
        m_overlayConfig.vertical = (line.find("true") != std::string::npos);

      auto readF = [&](const char *key, float &dst) {
        if (line.find(key) != std::string::npos) {
          try {
            dst = std::stof(getVal(line));
          } catch (...) {
          }
        }
      };
      readF("\"opacity\"", m_overlayConfig.opacity);
      readF("\"pos_x\"", m_overlayConfig.posX);
      readF("\"pos_y\"", m_overlayConfig.posY);
      readF("\"size_w\"", m_overlayConfig.sizeW);
      readF("\"size_h\"", m_overlayConfig.sizeH);
      readF("\"cpu_warn\"", m_overlayConfig.cpuWarn);
      readF("\"cpu_crit\"", m_overlayConfig.cpuCrit);
      readF("\"gpu_warn\"", m_overlayConfig.gpuWarn);
      readF("\"gpu_crit\"", m_overlayConfig.gpuCrit);
      readF("\"disk_warn\"", m_overlayConfig.diskWarn);
      readF("\"disk_crit\"", m_overlayConfig.diskCrit);
      
      if (line.find("\"preset_idx\"") != std::string::npos) {
        try { m_overlayConfig.presetIdx = std::stoi(getVal(line)); } catch (...) {}
      }
      if (line.find("\"cpu_ppt_cap\"") != std::string::npos) {
        try {
          m_overlayConfig.cpuPptCap = std::stoi(getVal(line));
          PowerControl::Get().SetCpuPowerLimitW(m_overlayConfig.cpuPptCap);
        } catch (...) {}
      }
      if (line.find("\"battery_limit\"") != std::string::npos) {
        try { m_overlayConfig.batteryLimit = std::stoi(getVal(line)); } catch (...) {}
      }
      if (line.find("\"amd_curve_optimizer\"") != std::string::npos) {
        try { 
          int val = std::stoi(getVal(line));
          if (val >= -30 && val <= 30)
            PowerControl::Get().SetCachedAmdCurveOptimizer(val);
        } catch (...) {}
      }
    }
  }
}


static int MapCurve(CurvePoint *points, float temp) {
  // Current Scale is 40-90 as requested

  if (temp <= points[0].temp)
    return points[0].speed;
  if (temp >= points[4].temp)
    return points[4].speed;

  for (int i = 0; i < 4; i++) {
    if (temp >= points[i].temp && temp <= points[i + 1].temp) {
      if (points[i + 1].temp == points[i].temp)
        return points[i].speed;
      float t = (temp - points[i].temp) /
                (float)(points[i + 1].temp - points[i].temp);
      return (int)(points[i].speed +
                   t * (points[i + 1].speed - points[i].speed));
    }
  }
  return points[4].speed;
}

void FanService::Update() {
  // 1. Read Raw RPMs (Throttled to 1s to prevent jumping)
  static auto lastRpmRead = std::chrono::steady_clock::now();
  auto nowRpm = std::chrono::steady_clock::now();

  if (std::chrono::duration_cast<std::chrono::milliseconds>(nowRpm -
                                                            lastRpmRead)
          .count() >= 1000) {
    float f1 = OmenEc::Get().GetFan1Speed();
    float f2 = OmenEc::Get().GetFan2Speed();

    static float smoothF1 = -1, smoothF2 = -1;
    if (smoothF1 < 0) {
      smoothF1 = f1;
      smoothF2 = f2;
    }

    float smoothFactor = 0.5f;
    smoothF1 = (smoothF1 * smoothFactor + f1 * (1.0f - smoothFactor));
    smoothF2 = (smoothF2 * smoothFactor + f2 * (1.0f - smoothFactor));

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_fan1Rpm = (smoothF1 > 0) ? smoothF1 : 0;
      m_fan2Rpm = (smoothF2 > 0) ? smoothF2 : 0;
    }
    lastRpmRead = nowRpm;
  }

  // 2. Control Logic
  FanControlMode mode = m_controlMode.load();
  static bool firstRun = true;
  if (firstRun) {
    firstRun = false;
  }

  if (mode != FanControlMode::Auto) {
    // Collect temperatures
    float curCpu = ThermalService::Get().GetCpuTemp();
    float curGpu = ThermalService::Get().GetGpuTemp();

    // EMA for jitter reduction (0.2 factor = heavy smoothing)
    m_avgCpu = (m_avgCpu * 0.8f + curCpu * 0.2f);
    m_avgGpu = (m_avgGpu * 0.8f + curGpu * 0.2f);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();

    // Recalculate targets every 2 seconds for smooth but responsive ramping
    if (now - m_lastTargetUpdate >= 2000 || m_lastTargetUpdate == 0) {
      if (mode == FanControlMode::AppMode) {
        float maxTemp = std::max(m_avgCpu, m_avgGpu);
        int cpuTarget = MapCurve(m_cpuCurve, m_avgCpu);
        int gpuTarget = MapCurve(m_gpuCurve, m_avgGpu);
        m_fan1Target = cpuTarget;
        m_fan2Target = gpuTarget;
      }
      m_lastTargetUpdate = now;
    }


    // --- Heartbeat Strategy ---
    // BIOS resets WMI control every ~80s.
    // The EC has a hardware watchdog of 30s (we set it to 0x1E).
    // If we ping the EC every 15s, we stay well within the 30s limit.
    // If the app freezes, the 30s hardware watchdog runs out, and fans safely go to Auto.
    static auto lastEcPing    = std::chrono::steady_clock::now();
    static auto lastFullAssert = std::chrono::steady_clock::now();
    auto nowHb = std::chrono::steady_clock::now();

    bool ecPingDue = std::chrono::duration_cast<std::chrono::milliseconds>(
                         nowHb - lastEcPing).count() >= 15000;
    bool fullAssertDue = std::chrono::duration_cast<std::chrono::seconds>(
                             nowHb - lastFullAssert).count() >= 60;

    if (ecPingDue) {
      OmenEc::Get().FanHeartbeat();       // cheap: keep EC control flag alive
      PowerControl::Get().ExtendFanCountdown();
      lastEcPing = nowHb;
    }

    // Apply/re-apply fan targets
    bool targetChanged = (m_fan1Target != m_lastAppliedFan1) ||
                         (m_fan2Target != m_lastAppliedFan2);

    m_persistenceCounter++;
    if (m_persistenceCounter >= 2) {
      m_persistenceCounter = 2;

      if (targetChanged || fullAssertDue) {
        // Full re-assert: EC registers + WMI (defeats BIOS reset)
        OmenEc::Get().SetFanMode(true);
        OmenEc::Get().SetFanSpeedPercent(0, m_fan1Target);
        OmenEc::Get().SetFanSpeedPercent(1, m_fan2Target);
        PowerControl::Get().SetFanLevelWmiBg(m_fan1Target, m_fan2Target);
        m_lastAppliedFan1 = m_fan1Target;
        m_lastAppliedFan2 = m_fan2Target;
        if (fullAssertDue) lastFullAssert = nowHb;
      } else if (ecPingDue) {
        // EC-only ping: re-enforce speed without WMI overhead
        OmenEc::Get().SetFanMode(true);
        OmenEc::Get().SetFanSpeedPercent(0, m_fan1Target);
        OmenEc::Get().SetFanSpeedPercent(1, m_fan2Target);
      }
    }
  } else {
    // Auto Mode
    m_persistenceCounter = 0;
    m_lastTargetUpdate = 0;
    m_lastAppliedFan1 = -1;
    m_lastAppliedFan2 = -1;
  }
}

float FanService::GetFanSpeed(int index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  return (index == 0) ? m_fan1Rpm : m_fan2Rpm;
}

void FanService::SetFanSpeed(int index, int percent) {
  m_controlMode = FanControlMode::Manual;
  OmenEc::Get().SetFanMode(true);

  if (index == 0)
    m_fan1Target = percent;
  else
    m_fan2Target = percent;

  OmenEc::Get().SetFanSpeedPercent(index, percent);
  PowerControl::Get().SetFanLevelWmi(m_fan1Target, m_fan2Target);
  SaveConfig();
}

void FanService::SetControlMode(FanControlMode mode) {
  m_controlMode = mode;
  SaveConfig();
}

void FanService::SetFanAuto() {
  m_controlMode = FanControlMode::Auto;
  OmenEc::Get().RestoreAutoControl();
  PowerControl::Get().RestoreFanAuto();
  SaveConfig();
}

float FanService::GetFanPercentage(int index) {
  if (index == 0)
    return (float)m_fan1Target;
  return (float)m_fan2Target;
}

void FanService::Heartbeat() {
  if (m_controlMode != FanControlMode::Auto)
    OmenEc::Get().FanHeartbeat();
}
