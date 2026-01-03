#pragma once
#include <atomic>
#include <mutex>

struct CurvePoint {
  int temp;
  int speed;
};

enum class FanControlMode { Auto, Manual, Sync, Optimized, Separated };

class FanService {
public:
  static FanService &Get();

  void Update(); // Called by HAL loop

  float GetFanSpeed(int index);
  void SetFanSpeed(int index, int percent);
  void SetFanAuto();

  float GetFanPercentage(int index);

  bool IsManualMode() const { return m_controlMode != FanControlMode::Auto; }
  FanControlMode GetControlMode() const { return m_controlMode; }
  void SetControlMode(FanControlMode mode);

  CurvePoint *GetCpuCurve() { return m_cpuCurve; }
  CurvePoint *GetGpuCurve() { return m_gpuCurve; }

  struct OverlayConfig {
    bool show = false;
    bool top = false;
    bool vertical = false;
    float opacity = 0.9f;
    float posX = 100.0f;
    float posY = 100.0f;
    // Temperature thresholds (orange / red)
    float cpuWarn = 70.0f;
    float cpuCrit = 80.0f;
    float gpuWarn = 60.0f;
    float gpuCrit = 80.0f;
    float diskWarn = 50.0f;
    float diskCrit = 60.0f;
    int batteryLimit = 100;
  };

  OverlayConfig &GetOverlayConfig() { return m_overlayConfig; }
  void SetOverlayConfig(const OverlayConfig &c) {
    m_overlayConfig = c;
    SaveConfig();
  }

  void SaveConfig();
  void LoadConfig();

  void Heartbeat();

private:
  FanService();

  std::mutex m_mutex;
  float m_fan1Rpm = 0.0f;
  float m_fan2Rpm = 0.0f;

  std::atomic<FanControlMode> m_controlMode{FanControlMode::Auto};

  CurvePoint m_cpuCurve[5];
  CurvePoint m_gpuCurve[5];

  int m_fan1Target = 0;
  int m_fan2Target = 0;
  int m_persistenceCounter = 0;

  // Hysteresis
  uint64_t m_lastTargetUpdate = 0;
  float m_avgCpu = 0.0f;
  float m_avgGpu = 0.0f;

  // Optimized mode: GPU fan speed offset (default 20% less than CPU)
  int m_optimizedGpuOffset = 20;

  // State tracking to reduce ACPI calls
  int m_lastAppliedFan1 = -1;
  int m_lastAppliedFan2 = -1;
  int m_heartbeatCounter = 0;

  OverlayConfig m_overlayConfig;
};
