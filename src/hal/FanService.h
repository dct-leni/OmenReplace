#pragma once
#include "WmiHelper.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

enum class FanControlMode { Auto = 0, AppMode = 2 };
enum class FanControlProfile { Default = 0, Quiet = 1, Cool = 2 };


class FanService {
public:
  static FanService &Get();

  void Update(); // Called by HAL loop

  float GetFanSpeed(int index);
  void SetFanAuto();

  float GetFanPercentage(int index);

  FanControlMode GetControlMode() const { return m_controlMode; }
  void SetControlMode(FanControlMode mode);

  FanControlProfile GetProfile() const { return m_profile.load(); }
  void SetProfile(FanControlProfile profile);

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
    int batteryLimit = 80;
    bool minimizeOnClose = true;
    bool logEnabled = false;
    // GPU power (TGP) override: -1=Auto, 0..2=Min/Med/Max
    int gpuPowerLevel = -1;
    bool autoPowerSwitch = false;  // Eco+Quiet on battery, restore on AC
    bool gameAutoProfile = false;  // Perf+Cool while a game is fullscreen
    // CPU temp (Tctl) limit via MP1 0x3F. 0 = Auto (firmware default).
    int tctlLimit = 90;
    // Embedded API configuration
    bool apiEnabled = true;
    int apiPort = 8080;
    bool apiBindAll = false;
    std::string apiToken = "";
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
  int m_heldPidTarget = -1; // deadband-held PID output (-1 = none yet)
  bool m_emergencyLatch = false; // latched 100% until temp drops 10° below limit
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
      // Default (Normal): Inaudible at idle, progressive linear ramp under load, 100% at high temps.
      static const CurvePoint kDefaultCurve[] = {
        { 40.0f, 18 }, // Whisper quiet idle
        { 50.0f, 25 },
        { 60.0f, 38 },
        { 70.0f, 52 },
        { 80.0f, 70 },
        { 88.0f, 88 },
        { 93.0f, 100 }
      };
      // Quiet: Low acoustics priority. Keeps fans <=45% up to 78°C, only ramps if approaching safety limits.
      static const CurvePoint kQuietCurve[] = {
        { 48.0f, 15 }, // Silent baseline
        { 58.0f, 22 },
        { 68.0f, 32 },
        { 78.0f, 45 },
        { 86.0f, 60 },
        { 92.0f, 80 },
        { 95.0f, 100 }
      };
      // Cool: Max cooling performance. Higher baseline pre-cooling and fast ramp to 100% at 80°C.
      static const CurvePoint kCoolCurve[] = {
        { 35.0f, 30 }, // High baseline pre-cooling
        { 45.0f, 45 },
        { 55.0f, 60 },
        { 65.0f, 75 },
        { 75.0f, 90 },
        { 80.0f, 100 }
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

      // Blend instantaneous die temperature with smoothed average.
      // If a sudden thermal spike occurs (current > avg + 3°C), react immediately; otherwise track smooth avg.
      float controlTemp = (currentTemp > avgTemp + 3.0f) ? (currentTemp - 1.5f) : avgTemp;
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
      // - Ramp-down is rate-limited to 2.5%/sec so fans decay smoothly without hunting/pulsing.
      if ((float)rawTarget > currentFan) {
        currentFan = (float)rawTarget;
      } else {
        float maxDrop = 2.5f * dt;
        currentFan = (std::max)(currentFan - maxDrop, (float)rawTarget);
      }

      return (int)std::round(std::clamp(currentFan, 0.0f, 100.0f));
    }
  };
  FanCurveEngine m_curveEngine;

  OverlayConfig m_overlayConfig;
};
