#include "OmenEc.h"
#include "LpcModuleData.h"
#include "MsrModuleData.h"
#include "OmenLog.h"
#include "SmbusModuleData.h"
#include "SmuModuleData.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

void LogEc(const std::string &msg) {}

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
  if (m_msrPawn)
    delete m_msrPawn;
  if (m_smbusPawn)
    delete m_smbusPawn;
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
  // LHM AMDFamily17 module (embedded): provides ioctl_read_msr (RAPL CPU
  // power). No runtime download — baked into the EXE.
  m_msrPawn = new PawnIO();
  if (!m_msrPawn->LoadBuffer(AMDFAMILY17_BIN, AMDFAMILY17_BIN_SIZE)) {
    LogEc("Failed to load embedded MSR module");
    delete m_msrPawn;
    m_msrPawn = nullptr;
  }
  // LHM SmbusPIIX4 module (embedded): provides ioctl_smbus_xfer (kernel SMBus
  // driver with port mux) for DIMM thermal reads.
  m_smbusPawn = new PawnIO();
  if (!m_smbusPawn->LoadBuffer(SMBUS_PIIX4_BIN, SMBUS_PIIX4_BIN_SIZE)) {
    LogEc("Failed to load embedded SMBus module");
    delete m_smbusPawn;
    m_smbusPawn = nullptr;
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
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; i++) {
    if ((ReadPort(0x66) & 0x02) == 0)
      return true;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count() > 150)
      return false;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return false;
}

uint8_t OmenEc::ReadByte(uint8_t offset) {
  for (int retry = 0; retry < 3; retry++) {
    if (!LockEc()) {
      Sleep(1);
      continue;
    }
    if (WaitEcInputEmpty()) {
      WritePort(0x66, 0x80);
      if (WaitEcInputEmpty()) {
        WritePort(0x62, offset);
        // This EC latches data on 0x62 without setting the OBF bit. Read it
        // directly after a short settle delay (verified: gives valid temps).
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        UnlockEc();
        return ReadPort(0x62);
      }
    }
    UnlockEc();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return 0;
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

float OmenEc::GetFan1Speed() {
  // Fan RPM tach not exposed in EC on this model — estimate from duty %.
  // ~100% duty ≈ 5800-6000 RPM (calibrated against real readings).
  float duty = (float)ReadByte(EC_XGS1);
  if (duty > 0.0f && duty <= 100.0f)
    return duty * 58.0f;
  return 0.0f;
}
float OmenEc::GetFan2Speed() {
  float duty = (float)ReadByte(EC_XGS2);
  if (duty > 0.0f && duty <= 100.0f)
    return duty * 58.0f;
  return 0.0f;
}

float OmenEc::GetCpuTemp57() { return (float)ReadByte(EC_CPUT); }
float OmenEc::GetCpuTemp58() { return (float)ReadByte(EC_RTMP); }
float OmenEc::GetGpuTemp() { return (float)ReadByte(EC_GPU_TEMP); }

bool OmenEc::TryReadMsr(uint32_t msr, std::vector<uint64_t> &out) {
  if (!m_msrPawn || !m_msrPawn->IsLoaded())
    return false;
  if (out.size() < 2) out.resize(2);
  std::vector<uint64_t> in = {(uint64_t)msr};
  // ioctl_read_msr takes [msr_index], returns one 64-bit value (eax|edx<<32)
  std::vector<uint64_t> result(1, 0);
  if (!m_msrPawn->Execute("ioctl_read_msr", in, result))
    return false;
  out[0] = result[0] & 0xFFFFFFFF;
  out[1] = (result[0] >> 32) & 0xFFFFFFFF;
  return true;
}

float OmenEc::GetCpuPackagePower() {
  // MSR RAPL energy counter: MSR_PWR_UNIT (0xC0010299) gives ESU scale,
  // MSR_PKG_ENERGY_STAT (0xC001029B) is a 32-bit energy counter.
  if (!m_msrPawn || !m_msrPawn->IsLoaded())
    return 0.0f;
  std::vector<uint64_t> unit(2), energy(2);
  if (!TryReadMsr(0xC0010299, unit))
    return 0.0f;
  if (!TryReadMsr(0xC001029B, energy))
    return 0.0f;
  // ESU [12:8]: energy unit = 15.3 uJ * 2^(ESU-16) per increment (AMD RAPL).
  int esu = (int)((unit[0] >> 8) & 0x1F);
  double baseUnit = 15.3 * std::pow(2.0, esu - 16); // uJ per increment
  uint32_t totalEnergy = (uint32_t)(energy[0] & 0xFFFFFFFF);

  uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  if (m_lastEnergyTime == 0 || m_lastEnergy == 0) {
    m_lastEnergy = totalEnergy;
    m_lastEnergyTime = nowMs;
    return 0.0f;
  }
  uint32_t delta = (totalEnergy >= m_lastEnergy)
                       ? (totalEnergy - m_lastEnergy)
                       : (0xFFFFFFFFu - m_lastEnergy) + totalEnergy;
  double dtSec = (nowMs - m_lastEnergyTime) / 1000.0;
  m_lastEnergy = totalEnergy;
  m_lastEnergyTime = nowMs;
  if (dtSec <= 0.0) return 0.0f;
  // energy (uJ) / dt (s) = uW; / 1e6 = W
  double watts = (baseUnit * (double)delta) / dtSec / 1e6;
  return (float)(watts > 0.0 && watts < 500.0 ? watts : 0.0);
}

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
  // Step 1: Disable fan boost (0xEC = 0x00)
  WriteByte(EC_FFFF, 0x00); 

  // Step 2: Release manual speed clamp (0xFF sentinel = unconfigured/hand control to BIOS)
  WriteByte(EC_XSS1, 0xFF); 
  WriteByte(EC_XSS2, 0xFF); 
  WriteByte(0x34, 0xFF); 
  WriteByte(0x35, 0xFF); 

  // Step 3: Enable fan hardware state (0xF4 = 0x00)
  WriteByte(EC_SFAN, 0x00); 

  // Step 4: Relinquish software control authority to BIOS (0x62 = 0x00)
  WriteByte(EC_OMCC, 0x00); 

  // Step 5: Zero countdown timer (0x63 = 0x00) so any active manual watchdog immediately expires
  WriteByte(EC_XFCD, 0x00); 

  Sleep(50); 

  // Step 6: Re-assert fan state and BIOS control authority to ensure hardware latch
  WriteByte(EC_SFAN, 0x00); 
  WriteByte(EC_OMCC, 0x00); 
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

  if (fanIndex == 0) {
    uint8_t valKrpm = (uint8_t)((percent * 55) / 100);
    WriteByte(EC_XSS1, valPercent);
    WriteByte(0x34, valKrpm); 
  } else {
    uint8_t valKrpm = (uint8_t)((percent * 57) / 100);
    WriteByte(EC_XSS2, valPercent);
    WriteByte(0x35, valKrpm); 
  }

  // EC_FFFF (0xEC) controls hardware Fan Boost: 0x0C = Max Boost (5500/5700 RPM), 0x00 = Regular
  if (percent >= 95) {
    WriteByte(EC_FFFF, 0x0C);
  } else {
    WriteByte(EC_FFFF, 0x00);
  }

  WriteByte(EC_OMCC, 0x06); 
  WriteByte(EC_XFCD, 0x1E); 
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
    for (size_t i = 0; i < 6 && i < out.size(); i++) {
      args[i] = (uint32_t)out[i];
    }
  }



  if (s_pciMutex) ReleaseMutex(s_pciMutex);

  return success;
}

// ─── MP1 (Power Management) SMU mailbox via raw SMN ─────────────────────────
// ioctl_send_smu_command only targets RSMU. MP1 commands (e.g. STAPM 0x4F)
// are sent by writing the MP1 mailbox registers directly through SMN.
// SMN addresses (ZenStates Zen4Settings): MSG=0x3B10530, RSP=0x3B1057C,
// ARG=0x3B109C4.
bool OmenEc::SendMp1Command(uint32_t cmd, uint32_t* args) {
  if (!m_smuPawn) return false;

  const uint32_t MP1_MSG = 0x3B10530;
  const uint32_t MP1_RSP = 0x3B1057C;
  const uint32_t MP1_ARG = 0x3B109C4;

  if (s_pciMutex) WaitForSingleObject(s_pciMutex, 1000);

  bool ok = false;
  uint32_t status = 0;

  // Clear response register.
  if (SmuWriteReg(MP1_RSP, 0)) {
    // Write up to 6 args.
    bool argsOk = true;
    for (int i = 0; i < 6; i++) {
      if (!SmuWriteReg(MP1_ARG + (uint32_t)(i * 4), args[i])) { argsOk = false; break; }
    }
    if (argsOk && SmuWriteReg(MP1_MSG, cmd)) {
      // Poll response register for completion (value != 0 == done).
      for (int i = 0; i < 200; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (SmuReadReg(MP1_RSP, status) && status != 0) { ok = true; break; }
      }
      // Read back args.
      for (int i = 0; i < 6 && ok; i++)
        SmuReadReg(MP1_ARG + (uint32_t)(i * 4), args[i]);
    }
  }

  if (s_pciMutex) ReleaseMutex(s_pciMutex);
  return ok;
}

// ─── SMBus DIMM thermal (via embedded SmbusPIIX4.bin) ──────────────────────
// Uses the LHM/ZenStates SmbusPIIX4 PawnIO module, which handles the FCH
// SMBus port mux + PIIX4 protocol in-kernel. ioctls:
//   ioctl_identity       → [type, ioBase, pciVidDid]
//   ioctl_piix4_port_sel → select SMBus port (DIMM thermal on KernCZ port)
//   ioctl_smbus_xfer     → [addr7, rw, cmd, protocol] word/byte data transfer

namespace {
constexpr long I2C_SMBUS_READ = 1;
constexpr long I2C_SMBUS_WRITE = 0;
constexpr long I2C_SMBUS_BYTE_DATA = 2; // protocol tag used by ioctl_smbus_xfer
constexpr long I2C_SMBUS_WORD_DATA = 3; // protocol tag used by ioctl_smbus_xfer
// KernCZ SMBus port mux: valid ports are {0,2,3,4}.
constexpr int SMBUS_PORTS[4] = {0, 2, 3, 4};
} // namespace

bool OmenEc::SmbusSelectPort(int port) {
  if (!m_smbusPawn || !m_smbusPawn->IsLoaded())
    return false;
  std::vector<uint64_t> portIn = {(uint64_t)port};
  std::vector<uint64_t> portOut(1, -1);
  return m_smbusPawn->Execute("ioctl_piix4_port_sel", portIn, portOut);
}

bool OmenEc::SmbusReadByte(uint8_t addr7, uint8_t cmd, uint8_t &val) {
  if (!m_smbusPawn || !m_smbusPawn->IsLoaded())
    return false;
  std::vector<uint64_t> in = {(uint64_t)addr7, I2C_SMBUS_READ, (uint64_t)cmd,
                              I2C_SMBUS_BYTE_DATA};
  std::vector<uint64_t> out(1, 0);
  if (!m_smbusPawn->Execute("ioctl_smbus_xfer", in, out))
    return false;
  val = (uint8_t)(out[0] & 0xFF);
  return true;
}

bool OmenEc::SmbusWriteByte(uint8_t addr7, uint8_t cmd, uint8_t value) {
  if (!m_smbusPawn || !m_smbusPawn->IsLoaded())
    return false;
  std::vector<uint64_t> in = {(uint64_t)addr7, I2C_SMBUS_WRITE, (uint64_t)cmd,
                              I2C_SMBUS_BYTE_DATA, (uint64_t)value};
  std::vector<uint64_t> out;
  return m_smbusPawn->Execute("ioctl_smbus_xfer", in, out);
}

bool OmenEc::EnableSmbusPci() {
  if (!m_smbusPawn || !m_smbusPawn->IsLoaded()) {
    OmenLog("[AMDOMEN] smbus module not loaded\n");
    return false;
  }

  // Probe identity to confirm the module sees the KernCZ SMBus controller.
  std::vector<uint64_t> in, out(3, 0);
  if (!m_smbusPawn->Execute("ioctl_identity", in, out)) {
    OmenLog("[AMDOMEN] smbus ioctl_identity failed\n");
    return false;
  }
  OmenLog("[AMDOMEN] smbus identity out_size=%d base=%d type=%d vid=0x%x\n",
          (int)out.size(), (int)(out.size() > 1 ? out[1] : 0),
          (int)(out.size() > 0 ? out[0] : 0),
          (int)(out.size() > 2 ? out[2] : 0));
  if (out.size() < 3 || out[1] == 0) // [1] = I/O base; 0 = not found
    return false;

  // Find the SMBus port that hosts the DIMMs by probing SPD EEPROMs (0x50-0x57)
  // on every valid KernCZ port {0,2,3,4}. DDR5 thermal sensor lives in the SPD.
  for (int pi = 0; pi < 4; pi++) {
    SmbusSelectPort(SMBUS_PORTS[pi]);
    for (int spd = 0x50; spd <= 0x57; spd++) {
      uint8_t b = 0;
      if (SmbusReadByte((uint8_t)spd, 0x00, b)) {
        m_smbusPort = SMBUS_PORTS[pi];
        m_smbusSpdAddr = (uint8_t)spd;
        OmenLog("[AMDOMEN] smbus SPD FOUND port=%d addr=0x%x byte0=0x%x\n",
                m_smbusPort, m_smbusSpdAddr, b);
        // DDR5: select page 0 via MREG_VIRTUAL_PAGE (0x0B), then check the
        // thermal sensor enabled bit (0x1A == 0 means enabled).
        SmbusWriteByte(m_smbusSpdAddr, 0x0B, 0x00);
        uint8_t en = 0xFF;
        if (SmbusReadByte(m_smbusSpdAddr, 0x1A, en)) {
          OmenLog("[AMDOMEN] smbus DDR5 thermal_enabled_reg=0x1A val=0x%x (%s)\n",
                  en, en == 0 ? "ENABLED" : "disabled");
        }
        return true;
      }
    }
  }
  OmenLog("[AMDOMEN] smbus no SPD found on any port\n");
  return false;
}

bool OmenEc::SmbusReadWord(uint8_t addr7, uint8_t cmd, uint16_t &val) {
  if (!m_smbusPawn || !m_smbusPawn->IsLoaded())
    return false;

  // ioctl_smbus_xfer: [addr7, rw=read, cmd, protocol] → [word value].
  std::vector<uint64_t> in = {(uint64_t)addr7, I2C_SMBUS_READ, (uint64_t)cmd,
                              I2C_SMBUS_WORD_DATA};
  std::vector<uint64_t> out(1, 0);
  if (!m_smbusPawn->Execute("ioctl_smbus_xfer", in, out)) {
    OmenLog("[AMDOMEN] smbus_xfer fail addr=0x%x cmd=0x%x\n", addr7, cmd);
    return false;
  }
  val = (uint16_t)(out[0] & 0xFFFF);
  return true;
}

float OmenEc::GetDimmTemp(int channel) {
  // DDR5 (8940HX): the DIMM thermal sensor lives in the SPD EEPROM itself.
  // SPD at 0x50/0x51, temp word at offset 0x31, resolution 0.0625°C (sign
  // bit 0x1000 → negative). Follows RAMSPDToolkit DDR5Accessor.
  if (channel < 0 || channel > 1) return 0.0f;
  if (m_smbusSpdAddr == 0) return 0.0f; // not initialized
  uint8_t spdAddr = (uint8_t)(m_smbusSpdAddr + (channel == 0 ? 0 : 1));
  // DDR5: only page 0 holds volatile temp data.
  SmbusWriteByte(spdAddr, 0x0B, 0x00);
  uint8_t en = 0xFF;
  if (!SmbusReadByte(spdAddr, 0x1A, en) || en != 0)
    return 0.0f; // thermal sensor disabled / absent
  uint16_t raw;
  if (SmbusReadWord(spdAddr, 0x31, raw)) {
    int sign = (raw & 0x1000) ? 1 : 0;
    float temp = (float)(raw & ~0x1000) * 0.0625f;
    if (sign) temp -= 256.0f;
    if (temp > 0 && temp < 100) return temp;
  }
  return 0.0f;
}
