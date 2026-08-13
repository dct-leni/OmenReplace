#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

enum class ApiAction {
  SetPowerMode,
  SetFanMode,
  SetFanProfile,
  SetBatteryLimit,
  SetAmdCo,
  FlushRam,
  SetGpuMode,
};

struct ApiCommand {
  ApiAction action;
  int intValue = 0;
};

class ApiServer {
public:
  static ApiServer &Get();

  void Start();
  void Stop();
  bool IsRunning() const { return m_running.load(); }

private:
  ApiServer();
  ~ApiServer();

  void ServerThreadFunc();
  void ControlWorkerThreadFunc();
  void EnqueueCommand(const ApiCommand &cmd);
  bool Authenticate(const std::string &authHeader, const std::string &queryToken) const;
  static std::string GenerateRandomToken();

  std::unique_ptr<httplib::Server> m_server;
  std::thread m_serverThread;
  std::thread m_workerThread;

  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  std::queue<ApiCommand> m_commandQueue;

  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stopWorker{false};

  int m_port = 8080;
  bool m_bindAll = false;
  std::string m_token;
};
