#include "OmenEc.h"
#include "LpcModuleData.h"
#include "SmuModuleData.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

void LogEc(const std::string &msg) {}

#include <fstream>
#include <iomanip>
#include <sstream>

static HANDLE s_ecMutex = NULL;
static HANDLE s_pciMutex = NULL;



OmenEc::OmenEc() {}
OmenEc::~OmenEc() {
  if (s_ecMutex)
    CloseHandle(s_ecMutex);
  if (m_pawn)
    delete m_pawn;
  if (m_smuPawn)
    delete m_smuPawn;
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
  s_pciMutex = CreateMutexA(NULL, FALSE, "Global\\Access_PCI");

  if (!PawnIO::InitLibrary()) {
    LogEc("PawnIO::InitLibrary failed");
    return false;
  }
  m_pawn = new PawnIO();

  if (!m_pawn->LoadBuffer(LPC_ACPI_EC_BIN, LPC_ACPI_EC_BIN_SIZE)) {
    LogEc("Failed to load embedded EC module");
    return false;
  }
  m_smuPawn = new PawnIO();
  if (!m_smuPawn->LoadBuffer(RYZEN_SMU_BIN, RYZEN_SMU_BIN_SIZE)) {
    LogEc("Failed to load embedded SMU module");
    return false;
  }
  LogEc("OmenEc initialized successfully");
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
    WriteByte(EC_OMCC, 0x06); 
    WriteByte(EC_XFCD, 0x1E); 
    WriteByte(EC_SFAN, 0x00); 
  } else {
    RestoreAutoControl();
  }
}

void OmenEc::RestoreAutoControl() {
  WriteByte(EC_XSS1, 0xFF);
  WriteByte(EC_XSS2, 0xFF);
  WriteByte(0x34, 0xFF); 
  WriteByte(0x35, 0xFF); 

  WriteByte(EC_OMCC, 0x00); 
  WriteByte(EC_XFCD, 0x00); 
  WriteByte(EC_FFFF, 0x00); 
  WriteByte(EC_SFAN, 0x00); 

  Sleep(100); 
}

void OmenEc::FanHeartbeat() {
  WriteByte(EC_XFCD, 0x1E); 
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
    WriteByte(0x34, valKrpm); 
  } else {
    WriteByte(EC_XSS2, valPercent);
    WriteByte(0x35, valKrpm); 
  }

  WriteByte(EC_OMCC, 0x06); 
  WriteByte(EC_XFCD, 0x1E); 
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
  if (!m_smuPawn) return false;
  // Fallback to SMU aperture if it's the SMU registers we're talking to
  if (bus == 0 && dev == 0 && func == 0 && (reg == 0xC4 || reg == 0xC8)) {
     std::vector<uint64_t> in = { (uint64_t)reg, (uint64_t)val };
     std::vector<uint64_t> out;
     return m_smuPawn->Execute("ioctl_write_smu_register", in, out);
  }
  return false;
}

bool OmenEc::PciReadDword(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t& val) {
  if (!m_smuPawn) return false;
  if (bus == 0 && dev == 0 && func == 0 && (reg == 0xC4 || reg == 0xC8)) {
     std::vector<uint64_t> in = { (uint64_t)reg };
     std::vector<uint64_t> out(1);
     if (m_smuPawn->Execute("ioctl_read_smu_register", in, out)) {
       val = (uint32_t)out[0];
       return true;
     }
  }
  return false;
}

bool OmenEc::SmuReadReg(uint32_t smnAddr, uint32_t& val) {
  if (!m_smuPawn) return false;
  std::vector<uint64_t> in = { (uint64_t)smnAddr };
  std::vector<uint64_t> out(1);
  if (m_smuPawn->Execute("ioctl_read_smu_register", in, out)) {
    val = (uint32_t)out[0];
    return true;
  }
  return false;
}

bool OmenEc::SmuWriteReg(uint32_t smnAddr, uint32_t val) {
  if (!m_smuPawn) return false;
  std::vector<uint64_t> in = { (uint64_t)smnAddr, (uint64_t)val };
  std::vector<uint64_t> out;
  return m_smuPawn->Execute("ioctl_write_smu_register", in, out);
}

bool OmenEc::SendSmuCommand(uint32_t msg, uint32_t* args) {
  if (!m_smuPawn) return false;

  // Sync hardware access to prevent collisions with other tools (Global\Access_PCI)
  if (s_pciMutex) WaitForSingleObject(s_pciMutex, 1000);

  std::vector<uint64_t> in = { (uint64_t)msg };
  for (int i = 0; i < 6; i++) in.push_back((uint64_t)args[i]);

  // IMPORTANT: out vector MUST be pre-sized to 6 for the driver to accept it
  std::vector<uint64_t> out(6, 0);
  bool success = m_smuPawn->Execute("ioctl_send_smu_command", in, out);

  if (success) {
    for (int i = 0; i < 6 && i < out.size(); i++) {
      args[i] = (uint32_t)out[i];
    }
  }



  if (s_pciMutex) ReleaseMutex(s_pciMutex);

  return success;
}
