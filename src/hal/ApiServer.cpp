#include <httplib.h>

#include "ApiServer.h"
#include "ApiDashboardHtml.h"
#include "FanService.h"
#include "OmenHal.h"
#include "OmenLog.h"
#include "PowerControl.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iomanip>
#include <objbase.h>
#include <random>
#include <sstream>

ApiServer &ApiServer::Get() {
  static ApiServer instance;
  return instance;
}

ApiServer::ApiServer() {}

ApiServer::~ApiServer() { Stop(); }

std::string ApiServer::GenerateRandomToken() {
  static const char hexChars[] = "0123456789abcdef";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 15);
  std::string token;
  token.reserve(32);
  for (int i = 0; i < 32; ++i) {
    token += hexChars[dis(gen)];
  }
  return token;
}

bool ApiServer::Authenticate(const std::string &authHeader,
                            const std::string &queryToken) const {
  if (m_token.empty()) return true;

  if (!queryToken.empty() && queryToken == m_token) return true;

  if (!authHeader.empty()) {
    if (authHeader.rfind("Bearer ", 0) == 0) {
      std::string bearer = authHeader.substr(7);
      if (bearer == m_token) return true;
    }
    if (authHeader == m_token) return true;
  }
  return false;
}

void ApiServer::EnqueueCommand(const ApiCommand &cmd) {
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_commandQueue.push(cmd);
  }
  m_queueCv.notify_one();
}

void ApiServer::ControlWorkerThreadFunc() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    OmenLog("[API] Control worker COM init failed hr=0x%08lx\n", (unsigned long)hr);
  } else {
    OmenLog("[API] Control worker COM init ok\n");
  }

  while (!m_stopWorker) {
    ApiCommand cmd;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueCv.wait(lock, [this] {
        return m_stopWorker.load() || !m_commandQueue.empty();
      });

      if (m_stopWorker && m_commandQueue.empty()) break;

      if (!m_commandQueue.empty()) {
        cmd = m_commandQueue.front();
        m_commandQueue.pop();
      } else {
        continue;
      }
    }

    // Execute the command in the serialized COM worker thread
    switch (cmd.action) {
    case ApiAction::SetPowerMode:
      OmenLog("[API] Executing SetPowerMode %d\n", cmd.intValue);
      OmenHal::Get().SetPowerMode(cmd.intValue);
      break;
    case ApiAction::SetFanMode:
      OmenLog("[API] Executing SetFanMode %d\n", cmd.intValue);
      if (cmd.intValue == 0) {
        OmenHal::Get().SetFanAuto();
      } else {
        FanService::Get().SetControlMode(FanControlMode::AppMode);
      }
      break;
    case ApiAction::SetFanProfile:
      OmenLog("[API] Executing SetFanProfile %d\n", cmd.intValue);
      FanService::Get().SetControlMode(FanControlMode::AppMode);
      FanService::Get().SetProfile((FanControlProfile)cmd.intValue);
      break;
    case ApiAction::SetBatteryLimit:
      OmenLog("[API] Executing SetBatteryLimit %d\n", cmd.intValue);
      OmenHal::Get().SetBatteryChargeLimit(cmd.intValue);
      break;
    case ApiAction::SetAmdCo:
      OmenLog("[API] Executing SetAmdCo %d\n", cmd.intValue);
      OmenHal::Get().SetAmdCurveOptimizer(cmd.intValue);
      FanService::Get().SaveConfig();
      break;
    case ApiAction::FlushRam:
      OmenLog("[API] Executing FlushRam\n");
      PowerControl::Get().FlushMemoryWorkingSet();
      OmenHal::Get().OptimizeMemory();
      break;
    case ApiAction::SetGpuMode:
      OmenLog("[API] Executing SetGpuMode %d\n", cmd.intValue);
      OmenHal::Get().RequestGpuMode(cmd.intValue);
      break;
    }
  }

  if (SUCCEEDED(hr)) CoUninitialize();
}

void ApiServer::Start() {
  if (m_running) return;

  auto &cfg = FanService::Get().GetOverlayConfig();
  if (!cfg.apiEnabled) {
    OmenLog("[API] Server disabled in config\n");
    return;
  }

  m_port = cfg.apiPort > 0 ? cfg.apiPort : 8080;
  m_bindAll = cfg.apiBindAll;
  
  if (cfg.apiToken.empty()) {
    cfg.apiToken = GenerateRandomToken();
    FanService::Get().SaveConfig();
    OmenLog("[API] Generated new secret API token\n");
  }
  m_token = cfg.apiToken;

  m_stopWorker = false;
  m_workerThread = std::thread(&ApiServer::ControlWorkerThreadFunc, this);

  m_running = true;
  m_serverThread = std::thread(&ApiServer::ServerThreadFunc, this);
  OmenLog("[API] Server starting on %s:%d\n", m_bindAll ? "0.0.0.0" : "127.0.0.1", m_port);
}

void ApiServer::Stop() {
  if (!m_running) return;

  OmenLog("[API] Server stopping\n");
  m_running = false;

  if (m_server) {
    m_server->stop();
  }

  if (m_serverThread.joinable()) {
    m_serverThread.join();
  }

  m_stopWorker = true;
  m_queueCv.notify_all();
  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }

  m_server.reset();
  OmenLog("[API] Server stopped\n");
}

void ApiServer::ServerThreadFunc() {
  m_server = std::make_unique<httplib::Server>();

  // CORS support
  m_server->set_default_headers({
      {"Access-Control-Allow-Origin", "*"},
      {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
      {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Token"}
  });

  m_server->Options(".*", [](const httplib::Request &, httplib::Response &res) {
    res.status = 204;
  });

  // 1. Embedded Web Dashboard
  m_server->Get("/", [](const httplib::Request &, httplib::Response &res) {
    res.set_content(OmenApi::GetDashboardHtml(), "text/html");
  });

  // Auth validator lambda
  auto checkAuth = [this](const httplib::Request &req, httplib::Response &res) -> bool {
    std::string authHdr;
    if (req.has_header("Authorization")) {
      authHdr = req.get_header_value("Authorization");
    } else if (req.has_header("X-API-Token")) {
      authHdr = req.get_header_value("X-API-Token");
    }
    std::string queryToken;
    if (req.has_param("token")) {
      queryToken = req.get_param_value("token");
    } else if (req.has_param("key")) {
      queryToken = req.get_param_value("key");
    }

    if (!Authenticate(authHdr, queryToken)) {
      res.status = 401;
      res.set_content("{\"error\":\"Unauthorized\",\"message\":\"Invalid or missing API token\"}",
                      "application/json");
      return false;
    }
    return true;
  };

  // 2. Real-time Telemetry
  m_server->Get("/api/telemetry", [this, checkAuth](const httplib::Request &req, httplib::Response &res) {
    if (!checkAuth(req, res)) return;

    auto &hal = OmenHal::Get();
    float cpuT = hal.GetCpuTemp();
    float gpuT = hal.GetGpuTemp();
    if (gpuT > 120.0f) gpuT = 0.0f;
    float ramT = hal.GetRamTemp();
    float ramTemp0 = 0, ramTemp1 = 0;
    int ramDimms = hal.GetRamTemps(ramTemp0, ramTemp1);
    float cpuL = hal.GetCpuLoad();
    float gpuL = hal.GetGpuLoad();
    float cpuP = hal.GetCpuPower();
    float gpuP = hal.GetGpuPower();
    float totalP = hal.GetTotalPower();
    float cpuVolts = PowerControl::Get().GetCpuVoltage();
    float ramUsed = 0, ramTotal = 0, ramPct = 0;
    PowerControl::Get().GetSystemRamUsage(ramUsed, ramTotal, ramPct);
    float fan1 = hal.GetFanSpeed(0);
    float fan2 = hal.GetFanSpeed(1);

    // Plain text format for microcontrollers / ESP32
    if (req.has_param("plain")) {
      char plainBuf[256];
      std::snprintf(plainBuf, sizeof(plainBuf),
                    "cpu=%.1f,gpu=%.1f,ram=%.1f,fan1=%.0f,fan2=%.0f,power=%.1f,cpupower=%.1f,gpupower=%.1f\n",
                    cpuT, gpuT, ramT, fan1, fan2, totalP > 0 ? totalP : 0.0f, cpuP, gpuP);
      res.set_content(plainBuf, "text/plain");
      return;
    }

    nlohmann::json j;
    j["cpu_temp"] = cpuT;
    j["gpu_temp"] = gpuT;
    j["ram_temp"] = ramT;
    j["ram_temp0"] = ramTemp0;
    j["ram_temp1"] = ramTemp1;
    j["ram_dimms"] = ramDimms;
    j["cpu_load"] = cpuL;
    j["gpu_load"] = gpuL;
    j["cpu_power"] = cpuP;
    j["gpu_power"] = gpuP;
    j["total_power"] = totalP > 0 ? totalP : 0.0f;
    j["cpu_volts"] = cpuVolts;
    j["ram_used_gb"] = ramUsed;
    j["ram_total_gb"] = ramTotal > 0 ? ramTotal : 32.0f;
    j["ram_pct"] = ramPct;
    j["fan1_rpm"] = fan1;
    j["fan2_rpm"] = fan2;
    j["cpu_name"] = hal.GetCpuName();
    j["gpu_name"] = hal.GetGpuName();

    auto drives = hal.GetDriveTemps();
    nlohmann::json dList = nlohmann::json::array();
    for (const auto &d : drives) {
      nlohmann::json dj;
      dj["model"] = d.Model;
      dj["temp"] = d.Temperature;
      dj["health"] = d.Health;
      dList.push_back(dj);
    }
    j["drives"] = dList;

    nlohmann::json state;
    state["power_mode"] = hal.GetPowerMode();
    state["fan_mode"] = (int)FanService::Get().GetControlMode();
    state["fan_profile"] = (int)FanService::Get().GetProfile();
    state["gpu_mux"] = hal.GetGpuModeInt();
    state["battery_limit"] = hal.GetBatteryChargeLimit();
    state["amd_co"] = hal.GetCachedAmdCurveOptimizer();
    j["state"] = state;

    res.set_content(j.dump(), "application/json");
  });

  // 3. State Endpoint
  m_server->Get("/api/state", [this, checkAuth](const httplib::Request &req, httplib::Response &res) {
    if (!checkAuth(req, res)) return;

    auto &hal = OmenHal::Get();
    nlohmann::json j;
    j["power_mode"] = hal.GetPowerMode();
    j["fan_mode"] = (int)FanService::Get().GetControlMode();
    j["fan_profile"] = (int)FanService::Get().GetProfile();
    j["gpu_mux"] = hal.GetGpuModeInt();
    j["battery_limit"] = hal.GetBatteryChargeLimit();
    j["amd_co"] = hal.GetCachedAmdCurveOptimizer();

    res.set_content(j.dump(), "application/json");
  });

  // 4. Control POST Endpoint
  m_server->Post("/api/control", [this, checkAuth](const httplib::Request &req, httplib::Response &res) {
    if (!checkAuth(req, res)) return;

    try {
      nlohmann::json j = nlohmann::json::parse(req.body);
      if (!j.contains("action")) {
        res.status = 400;
        res.set_content("{\"error\":\"Missing 'action' field\"}", "application/json");
        return;
      }

      std::string action = j["action"].get<std::string>();
      ApiCommand cmd;

      if (action == "set_power_mode") {
        cmd.action = ApiAction::SetPowerMode;
        cmd.intValue = std::clamp(j.value("value", 1), 0, 2);
      } else if (action == "set_fan_mode") {
        cmd.action = ApiAction::SetFanMode;
        cmd.intValue = j.value("value", 0);
      } else if (action == "set_fan_profile") {
        cmd.action = ApiAction::SetFanProfile;
        cmd.intValue = std::clamp(j.value("value", 0), 0, 2);
      } else if (action == "set_battery_limit") {
        cmd.action = ApiAction::SetBatteryLimit;
        int val = j.value("value", 100);
        cmd.intValue = (val <= 80) ? 80 : 100;
      } else if (action == "set_amd_co") {
        cmd.action = ApiAction::SetAmdCo;
        cmd.intValue = std::clamp(j.value("value", 0), -30, 0);
      } else if (action == "flush_ram") {
        cmd.action = ApiAction::FlushRam;
      } else if (action == "set_gpu_mode") {
        cmd.action = ApiAction::SetGpuMode;
        cmd.intValue = std::clamp(j.value("value", 0), 0, 1);
      } else {
        res.status = 400;
        res.set_content("{\"error\":\"Unknown action\"}", "application/json");
        return;
      }

      EnqueueCommand(cmd);
      res.set_content("{\"status\":\"ok\"}", "application/json");
    } catch (const std::exception &e) {
      res.status = 400;
      nlohmann::json err;
      err["error"] = "Invalid JSON payload";
      err["details"] = e.what();
      res.set_content(err.dump(), "application/json");
    }
  });

  const char *host = m_bindAll ? "0.0.0.0" : "127.0.0.1";
  if (!m_server->listen(host, m_port)) {
    OmenLog("[API] Server failed to listen on %s:%d\n", host, m_port);
  }
}
