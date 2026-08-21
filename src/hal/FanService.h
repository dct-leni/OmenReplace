#pragma once
#include "WmiHelper.h"
#include <atomic>
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
  int m_persistenceCounter = 0;

  // Hysteresis
  uint64_t m_lastTargetUpdate = 0;
  float m_avgCpu = 0.0f;
  float m_avgGpu = 0.0f;

  // State tracking to reduce ACPI calls
  int m_lastAppliedFan1 = -1;
  int m_lastAppliedFan2 = -1;
  std::atomic<bool> m_fanControlHealthy{false};

  // PID controller — profile-tuned closed-loop regulation
  // Calibrated against omencore / OmenXHub fan curves for 8940HX (Dragon Range,
  // TjMax ≈100°C, typical idle 45-55°C, gaming 75-95°C).
  struct FanPidController {
    float setpoint = 72.0f;
    float kp = 2.5f;
    float ki = 0.25f;
    float kd = 1.5f;
    int minSpeed = 20;  // % minimum baseline (profile-dependent)
    int maxSpeed = 100; // % cap (Quiet is capped at 85%)
    float lastError = 0.0f;
    float integral = 0.0f;
    uint64_t lastTime = 0;

    void ApplyPreset(FanControlProfile profile) {
      reset();
      if (profile == FanControlProfile::Quiet) {
        setpoint = 78.0f; kp = 1.5f; ki = 0.12f; kd = 0.8f;
        minSpeed = 15; maxSpeed = 85;
      } else if (profile == FanControlProfile::Cool) {
        setpoint = 65.0f; kp = 3.5f; ki = 0.4f; kd = 2.5f;
        minSpeed = 28; maxSpeed = 100;
      } else {
        setpoint = 72.0f; kp = 2.5f; ki = 0.25f; kd = 1.5f;
        minSpeed = 20; maxSpeed = 100;
      }
    }

    void reset() { lastTime = 0; integral = 0.0f; lastError = 0.0f; }

    int Compute(float temp, uint64_t nowMs) {
      if (lastTime == 0) {
        lastTime = nowMs;
        lastError = temp - setpoint;
        return minSpeed;
      }
      float dt = (nowMs - lastTime) / 1000.0f;
      if (dt < 0.5f) dt = 0.5f;
      if (dt > 3.0f) dt = 3.0f;

      float error = temp - setpoint;
      float p = kp * error;

      integral += error * dt;
      float iMax = 40.0f / (ki + 0.001f);
      float iMin = -10.0f / (ki + 0.001f);
      if (integral > iMax) integral = iMax;
      if (integral < iMin) integral = iMin;
      float i = ki * integral;

      float derivative = (error - lastError) / dt;
      float d = kd * derivative;

      lastError = error;
      lastTime = nowMs;

      float raw = p + i + d;
      int speed = minSpeed + (int)raw;
      if (speed < minSpeed) speed = minSpeed;
      if (speed > maxSpeed) speed = maxSpeed;
      return speed;
    }
  };
  FanPidController m_pid;

  OverlayConfig m_overlayConfig;
};
