#pragma once
#include "WmiHelper.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

#include <vector>

enum class FanControlMode { Auto = 0, AppMode = 2 };
enum class FanControlProfile { Default = 0, Quiet = 1, Cool = 2 };


class FanService {
public:
  static FanService &Get();

  void Update(); // Called by HAL loop

  float GetFanSpeed(int index);
  void SetFanAuto(bool persist = true);

  float GetFanPercentage(int index);

  FanControlMode GetControlMode() const { return m_controlMode; }
  void SetControlMode(FanControlMode mode, bool persist = true);

  FanControlProfile GetProfile() const { return m_profile.load(); }
  void SetProfile(FanControlProfile profile, bool persist = true);

  struct OverlayConfig {
    // HUD overlay settings
    bool show = false;
    bool hudPassthrough = true;
    float opacity = 0.8f;
    float posX = 100.0f;
    float posY = 100.0f;
    float sizeW = 180.0f;
    float sizeH = 135.0f;
    // System settings
    int powerMode = 1; // 0=Eco, 1=Balanced, 2=Perf
    int batteryLimit = 80;
    bool minimizeOnClose = true;
    bool logEnabled = false;
    // GPU power (TGP) override: -1=Auto, 0..2=Min/Med/Max
    int gpuPowerLevel = -1;
    bool autoPowerSwitch = false;  // Eco+Quiet on battery, restore on AC
    bool gameAutoProfile = false;  // Perf+Cool while a game is fullscreen
    // CPU temp (Tctl) limit via MP1 0x3F. 0 = Auto (firmware default).
    int tctlLimit = 90;
    // AMD All-Core Curve Optimizer (-30..0)
    int amdCurveOptimizer = 0;
    // Wake settings (Windows driver + HP BIOS S4/S5 & WLAN/BT)
    bool wakeOnLan = false;
    bool wakeOnWlanBt = false;
    // Embedded API configuration
    bool apiEnabled = true;
    int apiPort = 8080;
    bool apiBindAll = false;
    std::string apiToken = "";
    // Custom game directories for Game Compactor
    std::vector<std::wstring> customGameFolders;
  };

  OverlayConfig &GetOverlayConfig() { return m_overlayConfig; }

  void SaveConfig();
  void LoadConfig();

  // Worker-thread persistent WMI for fan RPM fallback
  static WmiHelper &WmiRpm() { return Get().m_wmiRpm; }

private:
  FanService();

  std::mutex m_mutex;
  std::mutex m_hardwareMutex;
  WmiHelper m_wmiRpm; // persistent WMI for fan RPM fallback (worker thread)
  float m_fan1Rpm = 0.0f;
  float m_fan2Rpm = 0.0f;

  std::atomic<FanControlMode> m_controlMode{FanControlMode::Auto};
  std::atomic<FanControlProfile> m_profile{FanControlProfile::Default};

  int m_fan1Target = 0;
  int m_fan2Target = 0;
  bool m_emergencyLatch = false; // latched 100% until temp drops below safety threshold
  int m_persistenceCounter = 0;

  // Hysteresis
  uint64_t m_lastTargetUpdate = 0;
  float m_avgCpu = 0.0f;
  float m_avgGpu = 0.0f;

  // State tracking to reduce ACPI calls
  int m_lastAppliedFan1 = -1;
  int m_lastAppliedFan2 = -1;
  std::atomic<bool> m_fanControlHealthy{false};

  // Curve Engine — Progressive multi-point thermal curves with asymmetric slew-rate
  // limiting and spike filtering. Calibrated for AMD Ryzen 8940HX / Phoenix / Dragon Range.
  struct CurvePoint {
    float temp;
    int fanPercent;
  };

  struct FanCurveEngine {
    FanControlProfile currentProfile = FanControlProfile::Default;
    float currentFan = 20.0f;
    uint64_t lastTimeMs = 0;

    void ApplyPreset(FanControlProfile profile) {
      currentProfile = profile;
      lastTimeMs = 0;
    }

    void Reset() {
      lastTimeMs = 0;
      currentFan = 20.0f;
    }

    static int Interpolate(float temp, const CurvePoint* curve, int count) {
      if (temp <= curve[0].temp) return curve[0].fanPercent;
      if (temp >= curve[count - 1].temp) return curve[count - 1].fanPercent;
      for (int i = 0; i < count - 1; ++i) {
        if (temp >= curve[i].temp && temp <= curve[i + 1].temp) {
          float t1 = curve[i].temp;
          float t2 = curve[i + 1].temp;
          float f1 = (float)curve[i].fanPercent;
          float f2 = (float)curve[i + 1].fanPercent;
          float ratio = (temp - t1) / (t2 - t1);
          return (int)std::round(f1 + (f2 - f1) * ratio);
        }
      }
      return curve[count - 1].fanPercent;
    }

    int Compute(float currentTemp, float avgTemp, uint64_t nowMs) {
      // 1. Curve tables:
      // Default (Normal): Inaudible at idle, progressive linear ramp under load, 100% at 85°C.
      static const CurvePoint kDefaultCurve[] = {
        { 40.0f, 18 }, // Whisper quiet idle
        { 55.0f, 30 },
        { 68.0f, 50 },
        { 78.0f, 75 },
        { 85.0f, 100 } // Full 5500/5700 RPM ceiling
      };
      // Quiet: Low acoustics priority. Keeps fans <=50% up to 80°C, only ramps if approaching safety limits.
      static const CurvePoint kQuietCurve[] = {
        { 48.0f, 15 }, // Silent baseline
        { 60.0f, 25 },
        { 72.0f, 40 },
        { 80.0f, 55 },
        { 88.0f, 80 },
        { 92.0f, 100 }
      };
      // Cool: Max cooling performance. Aggressive pre-cooling and full 100% (5500/5700 RPM) at 75°C.
      static const CurvePoint kCoolCurve[] = {
        { 35.0f, 35 }, // High baseline pre-cooling
        { 48.0f, 55 },
        { 60.0f, 75 },
        { 70.0f, 90 },
        { 75.0f, 100 } // Full 5500/5700 RPM max cooling
      };

      const CurvePoint* activeCurve = kDefaultCurve;
      int curvePoints = sizeof(kDefaultCurve) / sizeof(kDefaultCurve[0]);
      if (currentProfile == FanControlProfile::Quiet) {
        activeCurve = kQuietCurve;
        curvePoints = sizeof(kQuietCurve) / sizeof(kQuietCurve[0]);
      } else if (currentProfile == FanControlProfile::Cool) {
        activeCurve = kCoolCurve;
        curvePoints = sizeof(kCoolCurve) / sizeof(kCoolCurve[0]);
      }

      // Fast-track control temperature:
      // - In heavy load / high thermal zone (>=84°C), evaluate instantaneous die temp directly (zero lag).
      // - If a thermal jump occurs (current > avg + 2°C), track instantaneous die temp immediately.
      // - Otherwise follow smooth EMA average to prevent idle jitter.
      float controlTemp = (currentTemp >= 84.0f || currentTemp > avgTemp + 2.0f) ? currentTemp : avgTemp;
      int rawTarget = Interpolate(controlTemp, activeCurve, curvePoints);

      if (lastTimeMs == 0) {
        lastTimeMs = nowMs;
        currentFan = (float)rawTarget;
        return rawTarget;
      }

      float dt = (nowMs - lastTimeMs) / 1000.0f;
      if (dt < 0.2f) dt = 0.2f;
      if (dt > 3.0f) dt = 3.0f;
      lastTimeMs = nowMs;

      // Asymmetric Slew-Rate Limiting:
      // - Ramp-up is immediate to protect silicon from thermal heat-soak.
      // - Ramp-down is adaptive: decays quickly (5.0%/s) in the safe idle zone (<50°C),
      //   smoothly (3.5%/s) in the mid zone, and gradually (2.5%/s) under heavy thermal load.
      if ((float)rawTarget > currentFan) {
        currentFan = (float)rawTarget;
      } else {
        float decayRate = (controlTemp < 50.0f) ? 5.0f : (controlTemp < 68.0f ? 3.5f : 2.5f);
        float maxDrop = decayRate * dt;
        currentFan = (std::max)(currentFan - maxDrop, (float)rawTarget);
      }

      return (int)std::round(std::clamp(currentFan, 0.0f, 100.0f));
    }
  };
  FanCurveEngine m_curveEngine;

  OverlayConfig m_overlayConfig;
};
