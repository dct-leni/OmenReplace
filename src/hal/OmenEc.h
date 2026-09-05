#pragma once
#include <windows.h>
#include <cstdint>
#include "PawnIO.h"
#include <vector>

void LogEc(const std::string& msg);

// Hardware Abstraction for Omen Embedded Controller
// Implements fan speed reading and control
class OmenEc {
public:
    static OmenEc& Get();
    
    bool Initialize();
    bool IsInitialized() const { return m_initialized; }
    
    // High-level accessors
    float GetFan1Speed();
    float GetFan2Speed();
    float GetGpuTemp(); 

    // SMU Communication (AMD Ryzen)
    // Uses PCI config registers 0xC4/0xC8 to access SMN (System Management Network)
    bool SmuReadReg(uint32_t smnAddr, uint32_t& val);
    bool SmuWriteReg(uint32_t smnAddr, uint32_t val);
    // Send a command to the SMU via RSMU mailbox. args is 6-element array (in/out).
    bool SendSmuCommand(uint32_t cmd, uint32_t* args);
    // Send a command to the MP1 (Power Management) mailbox via raw SMN writes.
    // ioctl_send_smu_command only supports RSMU, so MP1 (e.g. STAPM 0x4F) is
    // done here. SMN addresses from ZenStates Zen4Settings: MSG=0x3B10530,
    // RSP=0x3B1057C, ARG=0x3B109C4. args is 6-element array (in/out).
    bool SendMp1Command(uint32_t cmd, uint32_t* args);
    
    // Fan Control
    void SetFanMode(bool manual);
    void SetFanSpeedPercent(int fanIndex, int percent);
    void FanHeartbeat(); // Pet the watchdog
    void RestoreAutoControl();
    
    float GetCpuTemp57(); // CPUT
    float GetCpuTemp58(); // RTMP
    
    uint8_t ReadByte(uint8_t offset);
    void WriteByte(uint8_t offset, uint8_t value);

    // SMBus DIMM thermal via embedded SmbusPIIX4.bin module (kernel SMBus
    // driver, handles FCH port mux). ioctls: ioctl_identity,
    // ioctl_piix4_port_sel, ioctl_smbus_xfer.
    bool EnableSmbusPci();                  // init module + find DIMM port
    bool SmbusSelectPort(int port);          // route port mux (valid: 0,2,3,4)
    bool SmbusReadByte(uint8_t addr7, uint8_t cmd, uint8_t &val);
    bool SmbusWriteByte(uint8_t addr7, uint8_t cmd, uint8_t value);
    bool SmbusReadWord(uint8_t addr7, uint8_t cmd, uint16_t &val); // word-data read
    // Probe DIMM thermal sensors (TSE2004). Returns °C for channel, 0 if none.
    float GetDimmTemp(int channel);

    // Try reading an MSR via PawnIO (RAPL energy counter for CPU power)
    bool TryReadMsr(uint32_t msr, std::vector<uint64_t> &out);
    // CPU package power via MSR RAPL energy counter (embedded AMDFamily17.bin).
    // Returns W, 0 if unavailable.
    float GetCpuPackagePower();

private:
    OmenEc();
    ~OmenEc();
    OmenEc(const OmenEc&) = delete;
    OmenEc& operator=(const OmenEc&) = delete;
    
    bool m_initialized = false;
    PawnIO* m_pawn = nullptr;
    PawnIO* m_smuPawn = nullptr;
    PawnIO* m_msrPawn = nullptr;   // embedded AMDFamily17 module (ioctl_read_msr)
    PawnIO* m_smbusPawn = nullptr; // embedded SmbusPIIX4 module (ioctl_smbus_xfer)
    int m_smbusPort = 0;           // SMBus port hosting the DIMMs
    uint8_t m_smbusSpdAddr = 0;    // SPD EEPROM addr (0x50-0x57) of DIMM 0
    uint32_t m_lastEnergy = 0;
    uint64_t m_lastEnergyTime = 0;
    
    // Helpers
    uint8_t ReadPort(uint16_t port);
    void WritePort(uint16_t port, uint8_t value);
    bool WaitEcInputEmpty();
    
    // HP Omen EC offsets (Dynamic based on model research)
    // Source of truth: external_source/omencore PawnIOEcAccess.cs + FanController.cs
    // NOTE: 0xB0-0xB3 are KEYBOARD RGB registers, NOT fan RPM!
    
    // Fan Control
    static const uint8_t EC_XSS1 = 0x2C; // L Fan Set Duty [%] (write)
    static const uint8_t EC_XSS2 = 0x2D; // R Fan Set Duty [%] (write)
    static const uint8_t EC_XGS1 = 0x2E; // L Fan Get Duty [%] (read)
    static const uint8_t EC_XGS2 = 0x2F; // R Fan Get Duty [%] (read)
    static const uint8_t EC_OMCC = 0x62; // Manual Fan Control toggle (0x06=Manual, 0x00=Auto)
    static const uint8_t EC_XFCD = 0x63; // Manual Fan Heartbeat [s] (0x1E=30s watchdog)
    static const uint8_t EC_FFFF = 0xEC; // Fan Boost (0x0C=ON, 0x00=OFF)
    static const uint8_t EC_SFAN = 0xF4; // Fan State (0x00=Enable, 0x02=Disable)

    // Temperature
    static const uint8_t EC_CPUT = 0x57;    // CPU Package Temperature (°C)
    static const uint8_t EC_RTMP = 0x58;    // CPU Core Temperature (°C)
    static const uint8_t EC_GPU_TEMP = 0xB7; // GPU Temperature (°C) — 0x59 wrong, 0xB7 correct (omencore+OmenCtl)
};
