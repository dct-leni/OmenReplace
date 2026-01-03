#pragma once
#include <windows.h>
#include <cstdint>
#include "PawnIO.h"

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
    float GetFan1Percentage(); // Reads 0x2E
    float GetFan2Percentage(); // Reads 0x2F
    float GetCpuTemp(); 
    float GetGpuTemp(); 

    // PCI Config Access via PawnIO
    bool PciWriteDword(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t val);
    bool PciReadDword(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t& val);
    
    // Fan Control
    void SetFanMode(bool manual);
    void SetFanSpeedPercent(int fanIndex, int percent);
    void FanHeartbeat(); // Pet the watchdog
    void RestoreAutoControl();
    
    float GetCpuTemp57(); // CPUT
    float GetCpuTemp58(); // RTMP
    
    // SSD Sensors (Device specific offsets)
    float GetSsd1Temp();
    float GetSsd2Temp();
    uint8_t ReadRegister(uint8_t addr) { return ReadByte(addr); }
    
    // Debug: Read a range of registers
    std::vector<uint8_t> ReadEcRange(uint8_t start, uint8_t count);
    
    uint8_t ReadByte(uint8_t offset);
    uint16_t ReadWord(uint8_t offset_l, uint8_t offset_h);
    void WriteByte(uint8_t offset, uint8_t value);
    
private:
    OmenEc();
    ~OmenEc();
    OmenEc(const OmenEc&) = delete;
    OmenEc& operator=(const OmenEc&) = delete;
    
    bool m_initialized = false;
    PawnIO* m_pawn = nullptr;
    
    // Helpers
    uint8_t ReadPort(uint16_t port);
    void WritePort(uint16_t port, uint8_t value);
    bool WaitEcInputEmpty();
    bool WaitEcOutputFull();
    
    // HP Omen EC offsets (Reverted to Little Endian)
    // HP Omen EC offsets (Dynamic based on model research)
    // HP Omen standard RPM registers (from OmenMon)
    static const uint8_t EC_RPM1_L = 0xB0; 
    static const uint8_t EC_RPM1_H = 0xB1;
    static const uint8_t EC_RPM2_L = 0xB2;
    static const uint8_t EC_RPM2_H = 0xB3;
    
    // RTMP=0x58, CPUT=0x57. 
    static const uint8_t EC_CPUT = 0x57; 
    static const uint8_t EC_RTMP = 0x58; 
    static const uint8_t EC_GPU_TEMP = 0x59; 
    static const uint8_t EC_EBPL = 0xD0;
    static const uint8_t EC_DBPL = 0xD6;
    
    // Fan Control (ck2 series optimized)
    // Fan Control
    static const uint8_t EC_XSS1 = 0x2C; // L Fan Set Speed [%]
    static const uint8_t EC_XSS2 = 0x2D; // R Fan Set Speed [%]
    static const uint8_t EC_SRP1 = 0x34; // L Fan Set Speed [krpm]
    static const uint8_t EC_SRP2 = 0x35; // R Fan Set Speed [krpm]
    static const uint8_t EC_XGS1 = 0x2E; // L Fan Get Speed [%] / Manual Override
    static const uint8_t EC_XGS2 = 0x2F; // R Fan Get Speed [%]
    static const uint8_t EC_OMCC = 0x62; // Manual Fan Control toggle
    static const uint8_t EC_XFCD = 0x63; // Manual Fan Heartbeat [s]
    static const uint8_t EC_HPCM = 0x95; // Performance Mode / Thermal Policy
    static const uint8_t EC_FFFF = 0xEC; // Max Fan Toggle
    static const uint8_t EC_SFAN = 0xF4; // Fan Toggle Switch
};
