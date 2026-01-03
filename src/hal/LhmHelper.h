#pragma once
#include "WmiHelper.h"
#include <string>
#include <vector>
#include <iostream>

struct LhmSensor {
    std::wstring Name;
    float Value;
    std::wstring Type;
    std::wstring Parent;
};

class LhmHelper {
public:
    WmiHelper m_wmi;
    bool m_connected = false;

    bool Initialize() {
        if (m_connected) return true;
        // Try connect to LHM namespace
        if (m_wmi.Initialize(L"ROOT\\LibreHardwareMonitor")) {
            m_connected = true;
            return true;
        }
        return false;
    }

    bool Update(float& cpuTemp, float& gpuTemp, float& fan1, float& fan2) {
        if (!m_connected) {
             if (!Initialize()) return false;
        }

        std::vector<LhmSensor> sensors;
        // We need Name, Value, SensorType
        // WmiHelper's ExecQuery usually gets one property. We need rows with multiple props.
        // It's easier to use ExecQueryAll for Names, then Values? No, inconsistent snapshot.
        // Let's modify WmiHelper or use raw WMI logic here?
        // Actually, let's just grab "Identifier" and "Value" and matching "SensorType"
        // But WmiHelper is simple string-variant.
        
        // Let's just fetch ALL sensors and parse locally?
        // Query: SELECT Name, Value, SensorType FROM Sensor
        
        // Since WmiHelper is limited, let's implement a specific query here via copy-paste-modify of raw WBEM?
        // Or extend WmiHelper.
        // Let's assume we can fetch ONE property "Value" where Name='...'.
        // But names vary ("Core (Tctl/Tdie)", "CPU Package").
        // We need to SCAN.
        return false; 
    }
    
    // Simpler approach: 
    // Get list of Names: SELECT Name FROM Sensor WHERE SensorType='Temperature'
    // Get list of Values: SELECT Value FROM Sensor WHERE SensorType='Temperature'
    // Map indices? Unreliable if order changes.
    
    // Better: Get "Identifier" which is unique (e.g. /amdcpu/0/temperature/0)
    // Then query Value.
};
