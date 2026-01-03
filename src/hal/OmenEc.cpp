#include "OmenEc.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

void LogEc(const std::string &msg) {}

static HANDLE s_ecMutex = NULL;

OmenEc::OmenEc() {}
OmenEc::~OmenEc() {
  if (s_ecMutex)
    CloseHandle(s_ecMutex);
  if (m_pawn)
    delete m_pawn;
}

OmenEc &OmenEc::Get() {
  static OmenEc instance;
  return instance;
}

bool OmenEc::Initialize() {
  if (m_initialized)
    return true;
  LogEc("=== Omen EC Initialization ===");
  s_ecMutex = CreateMutexA(NULL, FALSE, "Global\\Access_EC");
  if (!PawnIO::InitLibrary())
    return false;
  m_pawn = new PawnIO();

#include "LpcModuleData.h"
  if (!m_pawn->LoadBuffer(LPC_ACPI_EC_BIN, LPC_ACPI_EC_BIN_SIZE)) {
    LogEc("Failed to load embedded EC module");
    return false;
  }
  m_initialized = true;
  return true;
}

static bool LockEc() {
  if (!s_ecMutex)
    return true;
  DWORD res = WaitForSingleObject(s_ecMutex, 200);
  return (res == WAIT_OBJECT_0 || res == WAIT_ABANDONED);
}

static void UnlockEc() {
  if (s_ecMutex)
    ReleaseMutex(s_ecMutex);
}

uint8_t OmenEc::ReadPort(uint16_t port) {
  if (!m_pawn || !m_pawn->IsLoaded())
    return 0;
  std::vector<uint64_t> inputs = {port};
  std::vector<uint64_t> outputs(1);
  if (m_pawn->Execute("ioctl_pio_read", inputs, outputs))
    return (uint8_t)(outputs[0] & 0xFF);
  return 0;
}

void OmenEc::WritePort(uint16_t port, uint8_t value) {
  if (!m_pawn || !m_pawn->IsLoaded())
    return;
  std::vector<uint64_t> inputs = {port, value};
  std::vector<uint64_t> outputs;
  m_pawn->Execute("ioctl_pio_write", inputs, outputs);
}

bool OmenEc::WaitEcInputEmpty() {
  for (int i = 0; i < 2000; i++) {
    if ((ReadPort(0x66) & 0x02) == 0)
      return true;
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  return false;
}

bool OmenEc::WaitEcOutputFull() {
  for (int i = 0; i < 2000; i++) {
    if ((ReadPort(0x66) & 0x01) == 1)
      return true;
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  return false;
}

uint8_t OmenEc::ReadByte(uint8_t offset) {
  for (int retry = 0; retry < 3; retry++) {
    if (!LockEc())
      continue;
    uint8_t result = 0;
    bool success = false;
    if (WaitEcInputEmpty()) {
      WritePort(0x66, 0x80);
      if (WaitEcInputEmpty()) {
        WritePort(0x62, offset);
        if (WaitEcOutputFull()) {
          result = ReadPort(0x62);
          success = true;
        }
      }
    }
    UnlockEc();
    if (success)
      return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return 0;
}

uint16_t OmenEc::ReadWord(uint8_t offset_l, uint8_t offset_h) {
  if (!LockEc())
    return 0;

  auto readInternal = [&](uint8_t offset) -> uint8_t {
    uint8_t res = 0;
    if (WaitEcInputEmpty()) {
      WritePort(0x66, 0x80);
      if (WaitEcInputEmpty()) {
        WritePort(0x62, offset);
        if (WaitEcOutputFull()) {
          res = ReadPort(0x62);
        }
      }
    }
    return res;
  };

  uint8_t l = readInternal(offset_l);
  uint8_t h = readInternal(offset_h);

  UnlockEc();
  return (uint16_t)((h << 8) | l);
}

void OmenEc::WriteByte(uint8_t offset, uint8_t value) {
  if (!LockEc())
    return;
  if (WaitEcInputEmpty()) {
    WritePort(0x66, 0x81);
    if (WaitEcInputEmpty()) {
      WritePort(0x62, offset);
      if (WaitEcInputEmpty()) {
        WritePort(0x62, value);
      }
    }
  }
  UnlockEc();
}

float OmenEc::GetFan1Speed() { return (float)ReadWord(EC_RPM1_L, EC_RPM1_H); }
float OmenEc::GetFan2Speed() { return (float)ReadWord(EC_RPM2_L, EC_RPM2_H); }
float OmenEc::GetFan1Percentage() { return (float)ReadByte(EC_XGS1); }
float OmenEc::GetFan2Percentage() { return (float)ReadByte(EC_XGS2); }

float OmenEc::GetCpuTemp() { return (float)ReadByte(EC_CPUT); }
float OmenEc::GetCpuTemp57() { return (float)ReadByte(EC_CPUT); }
float OmenEc::GetCpuTemp58() { return (float)ReadByte(EC_RTMP); }
float OmenEc::GetGpuTemp() { return (float)ReadByte(EC_GPU_TEMP); }

void OmenEc::SetFanMode(bool manual) {
  if (manual) {
    // OmenMon Sequence: Manual = 0x06 (NOT 0x01!)
    WriteByte(EC_OMCC, 0x06); // Manual On (OmenMon value)
    // EC_XFCD (0x63) is the manual-fan countdown register.
    // We set it to 0x1E (30 seconds). If our app crashes and stops pinging,
    // the hardware will safely revert to auto mode after 30s.
    WriteByte(EC_XFCD, 0x1E); // 30s hardware watchdog
    WriteByte(EC_SFAN, 0x00); // Fan Switch On
  } else {
    RestoreAutoControl();
  }
}

void OmenEc::RestoreAutoControl() {
  // OmenMon Authoritative Sequence for BIOS Handover:
  // 1. Set fan speeds to MAX (0xFF) to signal "release"
  WriteByte(EC_XSS1, 0xFF);
  WriteByte(EC_XSS2, 0xFF);
  WriteByte(0x34, 0xFF); // SRP1
  WriteByte(0x35, 0xFF); // SRP2

  // 2. Clear all manual control toggles
  WriteByte(EC_OMCC, 0x00); // Manual Mode Off
  WriteByte(EC_XFCD, 0x00); // Heartbeat Off
  WriteByte(EC_FFFF, 0x00); // Max Fan Off
  WriteByte(EC_SFAN, 0x00); // Fan Switch On (for BIOS)

  Sleep(100); // Allow EC to process state change
}

// Heartbeat: reset the EC countdown to 30 seconds.
// We DO NOT disable the countdown here; if the app freezes, we want
// the hardware to safely revert to auto mode to prevent overheating.
void OmenEc::FanHeartbeat() {
  WriteByte(EC_XFCD, 0x1E); // 30s hardware watchdog re-arm
}

void OmenEc::SetFanSpeedPercent(int fanIndex, int percent) {
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;

  uint8_t valPercent = (uint8_t)percent;
  uint8_t valKrpm = (uint8_t)((percent * 55) / 100);

  if (fanIndex == 0) {
    WriteByte(EC_XSS1, valPercent);
    WriteByte(0x34, valKrpm); // SRP1
  } else {
    WriteByte(EC_XSS2, valPercent);
    WriteByte(0x35, valKrpm); // SRP2
  }

  // Pulse the apply bit to latch targets, then re-arm 30s heartbeat
  WriteByte(EC_OMCC, 0x06); // keep manual flag set (not just 0x01)
  WriteByte(EC_XFCD, 0x1E); // 30s watchdog
}

float OmenEc::GetSsd1Temp() { return (float)ReadByte(0x47); }
float OmenEc::GetSsd2Temp() { return (float)ReadByte(0x48); }

std::vector<uint8_t> OmenEc::ReadEcRange(uint8_t start, uint8_t count) {
  std::vector<uint8_t> data;
  for (int i = 0; i < count; i++) {
    data.push_back(ReadByte(start + i));
  }
  return data;
}
bool OmenEc::PciWriteDword(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t val) {
  if (!m_pawn) return false;
  std::vector<uint64_t> in = { (uint64_t)bus, (uint64_t)dev, (uint64_t)func, (uint64_t)reg, (uint64_t)val };
  std::vector<uint64_t> out;
  return m_pawn->Execute("ioctl_pci_write_config_dword", in, out);
}

bool OmenEc::PciReadDword(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t& val) {
  if (!m_pawn) return false;
  std::vector<uint64_t> in = { (uint64_t)bus, (uint64_t)dev, (uint64_t)func, (uint64_t)reg };
  std::vector<uint64_t> out = { 0 };
  if (m_pawn->Execute("ioctl_pci_read_config_dword", in, out) && !out.empty()) {
    val = (uint32_t)out[0];
    return true;
  }
  return false;
}
