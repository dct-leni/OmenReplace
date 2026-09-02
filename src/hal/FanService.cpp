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
  int saveMode = (int)PowerControl::Get().GetCurrentMode();
  if (saveMode == (int)PowerMode::Turbo) {
    saveMode = (int)PowerMode::Performance; // Turbo is a temporary active session mode
  }
  j["power_mode"] = saveMode;
  j["fan_mode"] = (int)m_controlMode.load();
  j["fan_profile"] = (int)m_profile.load();
  j["amd_curve_optimizer"] = m_overlayConfig.amdCurveOptimizer;
  j["battery_limit"] = m_overlayConfig.batteryLimit;
  j["display_overdrive"] = m_overlayConfig.displayOverdrive;
  j["minimize_on_close"] = m_overlayConfig.minimizeOnClose;
  j["log_enabled"] = m_overlayConfig.logEnabled;
  j["gpu_power_level"] = m_overlayConfig.gpuPowerLevel;
  j["auto_power_switch"] = m_overlayConfig.autoPowerSwitch;
  j["game_auto_profile"] = m_overlayConfig.gameAutoProfile;
  j["wake_on_lan"] = m_overlayConfig.wakeOnLan;
  j["wake_on_wlan_bt"] = m_overlayConfig.wakeOnWlanBt;

  nlohmann::json &hud = j["hud"];
  hud["show"] = m_overlayConfig.show;
  hud["passthrough"] = m_overlayConfig.hudPassthrough;
  hud["opacity"] = std::round((double)m_overlayConfig.opacity * 100.0) / 100.0;
  hud["pos_x"] = (int)std::round(m_overlayConfig.posX);
  hud["pos_y"] = (int)std::round(m_overlayConfig.posY);
  hud["size_w"] = (int)std::round(m_overlayConfig.sizeW);
  hud["size_h"] = (int)std::round(m_overlayConfig.sizeH);
  hud["tctl_limit"] = m_overlayConfig.tctlLimit;

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
      LogFanEvent("[AMDOMEN] config fan_mode=%d\n", mode);
    }

    if (j.contains("fan_profile")) {
      int prof = j["fan_profile"].get<int>();
      if (prof >= 0 && prof <= 2) {
        m_profile = static_cast<FanControlProfile>(prof);
        m_curveEngine.ApplyPreset(m_profile);
      }
    }

    int powerModeInt = 1;
    if (j.contains("power_mode")) {
      powerModeInt = j["power_mode"].get<int>();
      if (powerModeInt >= 0 && powerModeInt <= 2)
        m_overlayConfig.powerMode = powerModeInt;
      else if (powerModeInt == (int)PowerMode::Turbo)
        m_overlayConfig.powerMode = (int)PowerMode::Performance;
    }

    if (j.contains("battery_limit"))
      m_overlayConfig.batteryLimit = j["battery_limit"].get<int>();

    if (j.contains("display_overdrive"))
      m_overlayConfig.displayOverdrive = j["display_overdrive"].get<bool>();

    if (j.contains("minimize_on_close"))
      m_overlayConfig.minimizeOnClose = j["minimize_on_close"].get<bool>();

    if (j.contains("log_enabled"))
      m_overlayConfig.logEnabled = j["log_enabled"].get<bool>();

    if (j.contains("gpu_power_level")) {
      m_overlayConfig.gpuPowerLevel = j["gpu_power_level"].get<int>();
    }
    if (m_overlayConfig.gpuPowerLevel < 0 || m_overlayConfig.gpuPowerLevel > 2) {
      if (m_overlayConfig.powerMode == (int)PowerMode::Eco) m_overlayConfig.gpuPowerLevel = 0;
      else if (m_overlayConfig.powerMode == (int)PowerMode::Balanced) m_overlayConfig.gpuPowerLevel = 1;
      else m_overlayConfig.gpuPowerLevel = 2;
    }
    PowerControl::Get().SetGpuPowerOverride(m_overlayConfig.gpuPowerLevel);

    if (j.contains("auto_power_switch"))
      m_overlayConfig.autoPowerSwitch = j["auto_power_switch"].get<bool>();
    if (j.contains("game_auto_profile"))
      m_overlayConfig.gameAutoProfile = j["game_auto_profile"].get<bool>();
    if (j.contains("wake_on_lan"))
      m_overlayConfig.wakeOnLan = j["wake_on_lan"].get<bool>();
    if (j.contains("wake_on_wlan_bt"))
      m_overlayConfig.wakeOnWlanBt = j["wake_on_wlan_bt"].get<bool>();

    PowerControl::Get().SetAcEnabled(m_overlayConfig.autoPowerSwitch);

    if (j.contains("amd_curve_optimizer")) {
      int val = j["amd_curve_optimizer"].get<int>();
      // Support undervolt counts (-30..0), clamp cleanly
      if (val > 0) val = 0;
      if (val < -30) val = -30;
      m_overlayConfig.amdCurveOptimizer = val;
      PowerControl::Get().SetCachedAmdCurveOptimizer(val);
    }

    if (j.contains("hud")) {
      auto &h = j["hud"];
      if (h.contains("show")) m_overlayConfig.show = h["show"].get<bool>();
      if (h.contains("passthrough")) m_overlayConfig.hudPassthrough = h["passthrough"].get<bool>();
      if (h.contains("opacity")) m_overlayConfig.opacity = h["opacity"].get<float>();
      if (h.contains("pos_x")) m_overlayConfig.posX = h["pos_x"].get<float>();
      if (h.contains("pos_y")) m_overlayConfig.posY = h["pos_y"].get<float>();
      if (h.contains("size_w")) m_overlayConfig.sizeW = h["size_w"].get<float>();
      if (h.contains("size_h")) m_overlayConfig.sizeH = h["size_h"].get<float>();
      if (h.contains("tctl_limit")) {
        int v = h["tctl_limit"].get<int>();
        if (v != 0 && (v < 75 || v > 105))
          v = 0;
        m_overlayConfig.tctlLimit = v;
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
  if (sample < 0.0f || sample > 8000.0f)
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
    LogFanEvent("[AMDOMEN] fan_mode active=%d\n", (int)mode);
    lastLoggedMode = mode;
    modeWasLogged = true;
  }
  if (mode != FanControlMode::Auto) {
    if (!OmenHal::Get().IsFanControlReady()) {
      // Wait for background COM / WMI worker loop without resetting user mode preference
      return;
    }

    // Collect temperatures
    float curCpu = ThermalService::Get().GetCpuTemp();
    float curGpu = ThermalService::Get().GetGpuTemp();

    // EMA for jitter reduction (0.2 factor = heavy smoothing)
    m_avgCpu = (m_avgCpu * 0.8f + curCpu * 0.2f);
    m_avgGpu = (m_avgGpu * 0.8f + curGpu * 0.2f);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();

    // Recalculate targets every second
    if (now - m_lastTargetUpdate >= 1000 || m_lastTargetUpdate == 0) {
      if (mode == FanControlMode::AppMode) {
        float avgMax = (std::max)(m_avgCpu, m_avgGpu);
        float curMax = (std::max)(curCpu, curGpu);

        // Thermal safety threshold derives from the user's CPU temp limit
        float tctlLimit = (float)FanService::Get().GetOverlayConfig().tctlLimit;
        float emergencyTemp = (tctlLimit > 0.0f) ? tctlLimit - 2.0f : 93.0f;
        if (m_emergencyLatch) {
          if (avgMax < emergencyTemp - 4.0f && curMax < emergencyTemp - 3.0f)
            m_emergencyLatch = false;
        } else if (avgMax >= emergencyTemp || curMax >= emergencyTemp + 1.0f) {
          m_emergencyLatch = true;
        }

        if (m_emergencyLatch) {
          m_fan1Target = 100;
          m_fan2Target = 100;
        } else {
          int target = m_curveEngine.Compute(curMax, avgMax, now);
          m_fan1Target = target;
          m_fan2Target = target;
        }
        LogFanEvent("[AMDOMEN] fan_targets cpu=%d gpu=%d\n", m_fan1Target,
                    m_fan2Target);
      }
      m_lastTargetUpdate = now;
    }

    // --- AMD EC Keepalive Strategy ---
    // The AMD EC has a 3-second hardware watchdog. Refresh targets every 1s
    // unconditionally so the hardware never drops back to the internal BIOS table.
    static auto lastAssertTime = std::chrono::steady_clock::now();
    auto nowAssert = std::chrono::steady_clock::now();
    int64_t msSinceAssert = std::chrono::duration_cast<std::chrono::milliseconds>(
                                nowAssert - lastAssertTime).count();

    bool targetChanged = (m_fan1Target != m_lastAppliedFan1) ||
                         (m_fan2Target != m_lastAppliedFan2);

    if (targetChanged || msSinceAssert >= 1000) {
      bool wmiOk = FanController::Get().AssertTargets(m_fan1Target, m_fan2Target);
      LogFanEvent("[AMDOMEN] fan_write cpu=%d gpu=%d wmi_ok=%d\n",
                  m_fan1Target, m_fan2Target, wmiOk ? 1 : 0);
      m_fanControlHealthy = wmiOk;
      m_lastAppliedFan1 = m_fan1Target;
      m_lastAppliedFan2 = m_fan2Target;
      lastAssertTime = nowAssert;
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
  LogFanEvent("[AMDOMEN] fan_mode requested=%d\n", (int)normalized);
  SaveConfig();
}

void FanService::SetFanAuto() {
  m_controlMode = FanControlMode::Auto;
  m_fanControlHealthy = false;
  LogFanEvent("[AMDOMEN] fan_mode forced=0\n");
  FanController::Get().RestoreBios();
  SaveConfig();
}

void FanService::SetProfile(FanControlProfile profile) {
  m_profile = profile;
  m_curveEngine.ApplyPreset(profile);
  LogFanEvent("[AMDOMEN] fan_profile=%d\n", (int)profile);
  SaveConfig();
}

float FanService::GetFanPercentage(int index) {
  if (index == 0)
    return (float)m_fan1Target;
  return (float)m_fan2Target;
}
