#pragma once

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <windows.h>

// Logging is disabled by default; enabled via config "overlay.log_enabled".
// main() sets the flag from config once loaded.

inline std::mutex &OmenLogMutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::string OmenLogPath() {
  char path[MAX_PATH] = {};
  DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
    return "omen_control.log";

  std::string result(path, length);
  size_t separator = result.find_last_of("\\/");
  if (separator == std::string::npos)
    return "omen_control.log";
  return result.substr(0, separator + 1) + "omen_control.log";
}

inline bool &OmenLogEnabledFlag() {
  static bool enabled = false;
  return enabled;
}

// Enable/disable logging (read from config by main()).
inline void OmenLogSetEnabled(bool on) { OmenLogEnabledFlag() = on; }

inline void OmenLogStart() {
  if (!OmenLogEnabledFlag()) return;
  std::lock_guard<std::mutex> lock(OmenLogMutex());
  std::ofstream log(OmenLogPath(), std::ios::trunc);
  if (!log.is_open())
    return;

  SYSTEMTIME now;
  GetLocalTime(&now);
  log << "\n=== OMEN Control start " << now.wYear << "-" << now.wMonth << "-"
      << now.wDay << " " << now.wHour << ":" << now.wMinute << ":"
      << now.wSecond << "." << now.wMilliseconds << " ===\n";
}

inline void OmenLog(const char *format, ...) {
  if (!OmenLogEnabledFlag()) return;
  char message[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  std::lock_guard<std::mutex> lock(OmenLogMutex());
  std::ofstream log(OmenLogPath(), std::ios::app);
  if (!log.is_open())
    return;

  SYSTEMTIME now;
  GetLocalTime(&now);
  log << "[" << now.wHour << ":" << now.wMinute << ":" << now.wSecond
      << "." << now.wMilliseconds << "] " << message;
}
