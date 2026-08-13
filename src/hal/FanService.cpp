#include "FanService.h"
#include "FanController.h"
#include "OmenEc.h"
#include "OmenHal.h"
#include "OmenLog.h"
#include "PowerControl.h"
#include <nlohmann/json.hpp>
#include "ThermalService.h"
#include "TelemetryService.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <windows.h>

static void LogFanEvent(const char *format, int first = 0, int second = 0,
                        int third = 0) {
  char message[160];
  std::snprintf(message, sizeof(message), format, first, second, third);
  OmenLog("%s", message);
}

// Resolve config.json next to the EXE (not the CWD, which can be System32).
static std::string ConfigPath() {
  char path[MAX_PATH] = {};
  DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
    return "config.json";
  std::string result(path, length);
  size_t sep = result.find_last_of("\\/");
  if (sep == std::string::npos)
    return "config.json";
  return result.substr(0, sep + 1) + "config.json";
}

FanService &FanService::Get() {
  static FanService instance;
  return instance;
}

#include <fstream>

FanService::FanService() {
  LoadConfig();
}

void FanService::SaveConfig() {
  nlohmann::json j;
  j["power_mode"] = (int)PowerControl::Get().GetCurrentMode();
  j["fan_mode"] = (int)m_controlMode.load();
  j["fan_profile"] = (int)m_profile.load();
  j["amd_curve_optimizer"] = PowerControl::Get().GetCachedAmdCurveOptimizer();
  j["battery_limit"] = m_overlayConfig.batteryLimit;
  j["minimize_on_close"] = m_overlayConfig.minimizeOnClose;

  nlohmann::json &hud = j["hud"];
  hud["show"] = m_overlayConfig.show;
  hud["passthrough"] = m_overlayConfig.hudPassthrough;
  hud["opacity"] = (float)(std::round(m_overlayConfig.opacity * 100.0f) / 100.0f);
  hud["pos_x"] = (int)std::round(m_overlayConfig.posX);
  hud["pos_y"] = (int)std::round(m_overlayConfig.posY);
  hud["size_w"] = (int)std::round(m_overlayConfig.sizeW);
  hud["size_h"] = (int)std::round(m_overlayConfig.sizeH);

  nlohmann::json &api = j["api"];
  api["enabled"] = m_overlayConfig.apiEnabled;
  api["port"] = m_overlayConfig.apiPort;
  api["bind_all"] = m_overlayConfig.apiBindAll;
  api["token"] = m_overlayConfig.apiToken;

  std::ofstream f(ConfigPath());
  if (f.is_open())
    f << j.dump(2);
}

void FanService::LoadConfig() {
  std::ifstream f(ConfigPath());
  if (!f.is_open())
    return;

  try {
    nlohmann::json j = nlohmann::json::parse(f);

    if (j.contains("fan_mode")) {
      int mode = j["fan_mode"].get<int>();
      if (mode != (int)FanControlMode::Auto &&
          mode != (int)FanControlMode::AppMode)
        mode = (int)FanControlMode::Auto;
      m_controlMode = static_cast<FanControlMode>(mode);
      LogFanEvent("[OMEN] config fan_mode=%d\n", mode);
    }

    if (j.contains("fan_profile")) {
      int prof = j["fan_profile"].get<int>();
      if (prof >= 0 && prof <= 2)
        m_profile = static_cast<FanControlProfile>(prof);
    }

    if (j.contains("power_mode"))
      PowerControl::Get().SetMode(
          static_cast<PowerMode>(j["power_mode"].get<int>()));

    if (j.contains("battery_limit"))
      m_overlayConfig.batteryLimit = j["battery_limit"].get<int>();

    if (j.contains("minimize_on_close"))
      m_overlayConfig.minimizeOnClose = j["minimize_on_close"].get<bool>();

    if (j.contains("amd_curve_optimizer")) {
      int val = j["amd_curve_optimizer"].get<int>();
      if (val >= -30 && val <= 30)
        PowerControl::Get().SetCachedAmdCurveOptimizer(val);
    }

    // Modern "hud" section with fallback to legacy "overlay"
    const char *hudKey = j.contains("hud") ? "hud" : (j.contains("overlay") ? "overlay" : nullptr);
    if (hudKey) {
      auto &h = j[hudKey];
      if (h.contains("show")) m_overlayConfig.show = h["show"].get<bool>();
      if (h.contains("passthrough")) m_overlayConfig.hudPassthrough = h["passthrough"].get<bool>();
      else if (h.contains("hud_passthrough")) m_overlayConfig.hudPassthrough = h["hud_passthrough"].get<bool>();
      if (h.contains("opacity")) m_overlayConfig.opacity = h["opacity"].get<float>();
      if (h.contains("pos_x")) m_overlayConfig.posX = h["pos_x"].get<float>();
      if (h.contains("pos_y")) m_overlayConfig.posY = h["pos_y"].get<float>();
      if (h.contains("size_w")) m_overlayConfig.sizeW = h["size_w"].get<float>();
      if (h.contains("size_h")) m_overlayConfig.sizeH = h["size_h"].get<float>();

      // Legacy fallbacks if nested inside overlay
      if (!j.contains("battery_limit") && h.contains("battery_limit"))
        m_overlayConfig.batteryLimit = h["battery_limit"].get<int>();
      if (!j.contains("minimize_on_close") && h.contains("minimize_on_close"))
        m_overlayConfig.minimizeOnClose = h["minimize_on_close"].get<bool>();
      if (!j.contains("amd_curve_optimizer") && h.contains("amd_curve_optimizer")) {
        int val = h["amd_curve_optimizer"].get<int>();
        if (val >= -30 && val <= 30)
          PowerControl::Get().SetCachedAmdCurveOptimizer(val);
      }
    }

    if (j.contains("api")) {
      auto &api = j["api"];
      if (api.contains("enabled"))
        m_overlayConfig.apiEnabled = api["enabled"].get<bool>();
      if (api.contains("port"))
        m_overlayConfig.apiPort = api["port"].get<int>();
      if (api.contains("bind_all"))
        m_overlayConfig.apiBindAll = api["bind_all"].get<bool>();
      if (api.contains("token"))
        m_overlayConfig.apiToken = api["token"].get<std::string>();
    }
  } catch (...) {
  }
}


// Primary RPM source: WMI BIOS CMD 0x2D (GetFanLevel V1), which returns real
// fan levels in krpm units (level 44 = 4400 RPM) and works across thermal
// policies. The EC RPM registers are unpopulated on this model (GetFan1Speed
// returns duty*50), so 0x2D is preferred over EC. Command 0x38 (V2) is
// OMEN Max 2025+ only and fails here.
static float ReadStableRpm(bool secondFan) {
  auto &wmi = FanService::WmiRpm();
  if (wmi.Initialize()) {
    uint8_t d[4] = {0, 0, 0, 0};
    std::vector<uint8_t> out;
    if (wmi.ExecuteHpBiosMethod(0x20008, 0x2D, d, 4, out, 128)) {
      if (out.size() >= 2) {
        int f1 = out[0] * 100;
        int f2 = out[1] * 100;
        int rpm = secondFan ? f2 : f1;
        if (rpm >= 0 && rpm <= 8000)
          return (float)rpm;
      }
    }
  }

  // WMI unavailable/failed — fall back to EC RPM registers (duty*50 estimate).
  float samples[3] = {};
  for (float &sample : samples) {
    sample = secondFan ? OmenEc::Get().GetFan2Speed()
                       : OmenEc::Get().GetFan1Speed();
  }
  std::sort(std::begin(samples), std::end(samples));
  float median = samples[1];
  if (median > 0.0f && median <= 10000.0f)
    return median;

  return -1.0f;
}

static float RejectRpmJump(float sample, float previous) {
  if (sample < 0.0f)
    return previous;
  if (previous >= 0.0f && previous > 0.0f && sample > 0.0f &&
      std::abs(sample - previous) > 1000.0f)
    return previous;
  return sample;
}

void FanService::Update() {
  // 1. Read Raw RPMs (Throttled to 1s to prevent jumping)
  static auto lastRpmRead = std::chrono::steady_clock::now();
  auto nowRpm = std::chrono::steady_clock::now();

  if (std::chrono::duration_cast<std::chrono::milliseconds>(nowRpm -
                                                            lastRpmRead)
          .count() >= 1000) {
    static float lastRawF1 = -1.0f;
    static float lastRawF2 = -1.0f;
    float f1 = RejectRpmJump(ReadStableRpm(false), lastRawF1);
    float f2 = RejectRpmJump(ReadStableRpm(true), lastRawF2);
    lastRawF1 = f1;
    lastRawF2 = f2;

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

    // Telemetry logging (compiled out unless OMEN_TELEMETRY defined)
    TelemetryService::Get().Update(
        ThermalService::Get().GetCpuTemp(), ThermalService::Get().GetGpuTemp(),
        m_fan1Rpm, m_fan2Rpm, ThermalService::Get().GetCpuLoad(),
        ThermalService::Get().GetGpuLoad(), ThermalService::Get().GetTotalPower());

    lastRpmRead = nowRpm;
  }

  // 2. Control Logic
  FanControlMode mode = m_controlMode.load();
  static FanControlMode lastLoggedMode = FanControlMode::Auto;
  static bool modeWasLogged = false;
  if (!modeWasLogged || mode != lastLoggedMode) {
    LogFanEvent("[OMEN] fan_mode active=%d\n", (int)mode);
    lastLoggedMode = mode;
    modeWasLogged = true;
  }
  if (mode != FanControlMode::Auto && !OmenHal::Get().IsFanControlReady()) {
    // Never assert manual EC/WMI control until worker COM and WMI are ready.
    m_controlMode = FanControlMode::Auto;
    mode = FanControlMode::Auto;
    LogFanEvent("[OMEN] fan_control readiness_failed mode=%d\n", (int)mode);
    FanController::Get().RestoreBios();
    m_lastAppliedFan1 = -1;
    m_lastAppliedFan2 = -1;
  }
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
        // Thermal safety: force 100% fan above 90°C regardless of profile.
        if (maxTemp > 90.0f) {
          m_fan1Target = 100;
          m_fan2Target = 100;
        } else {
          int cpuTarget = m_pid.Compute(maxTemp, now);
          m_fan1Target = cpuTarget;
          m_fan2Target = std::max(m_pid.minSpeed, cpuTarget - 10);
        }
        LogFanEvent("[OMEN] fan_targets cpu=%d gpu=%d\n", m_fan1Target,
                    m_fan2Target);
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
      // EC watchdog is reliable on this firmware. WMI command 0x31 fails
      // consistently while fan-level command 0x2E succeeds, so do not make
      // active control depend on unsupported countdown renewal.
      FanController::Get().Heartbeat();
      LogFanEvent("[OMEN] fan_heartbeat ec_sent=1 wmi_countdown=disabled\n");
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
        bool wmiOk = FanController::Get().AssertTargets(m_fan1Target,
                                                        m_fan2Target);
        LogFanEvent("[OMEN] fan_write cpu=%d gpu=%d wmi_ok=%d\n",
                    m_fan1Target, m_fan2Target, wmiOk ? 1 : 0);
        m_fanControlHealthy = wmiOk;
        m_lastAppliedFan1 = m_fan1Target;
        m_lastAppliedFan2 = m_fan2Target;
        if (fullAssertDue) lastFullAssert = nowHb;
      } else if (ecPingDue) {
        // EC-only ping: re-enforce speed without WMI overhead
        FanController::Get().ReassertEc(m_fan1Target, m_fan2Target);
      }
    }
  } else {
    // Auto Mode
    m_persistenceCounter = 0;
    m_lastTargetUpdate = 0;
      m_lastAppliedFan1 = -1;
      m_lastAppliedFan2 = -1;
      m_fanControlHealthy = false;
  }
}

float FanService::GetFanSpeed(int index) {
  std::lock_guard<std::mutex> lock(m_mutex);
  return (index == 0) ? m_fan1Rpm : m_fan2Rpm;
}

void FanService::SetControlMode(FanControlMode mode) {
  FanControlMode normalized = (mode == FanControlMode::AppMode)
                                  ? FanControlMode::AppMode
                                  : FanControlMode::Auto;
  m_controlMode = normalized;
  LogFanEvent("[OMEN] fan_mode requested=%d\n", (int)normalized);
  SaveConfig();
}

void FanService::SetFanAuto() {
  m_controlMode = FanControlMode::Auto;
  m_fanControlHealthy = false;
  LogFanEvent("[OMEN] fan_mode forced=0\n");
  FanController::Get().RestoreBios();
  SaveConfig();
}

void FanService::SetProfile(FanControlProfile profile) {
  m_profile = profile;
  m_pid.ApplyPreset(profile);
  LogFanEvent("[OMEN] fan_profile=%d\n", (int)profile);
  SaveConfig();
}

float FanService::GetFanPercentage(int index) {
  if (index == 0)
    return (float)m_fan1Target;
  return (float)m_fan2Target;
}
