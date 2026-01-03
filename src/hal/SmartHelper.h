#pragma once
#include <windows.h>
#include <vector>
#include <string>

struct DriveInfo {
    int Index;
    std::string Model;
    int Temperature; // Celsius
};

class SmartHelper {
public:
    SmartHelper();
    ~SmartHelper();

    void ScanDrives();
    const std::vector<DriveInfo>& GetDrives() const { return m_drives; }
    void UpdateTemps();

private:
    std::vector<DriveInfo> m_drives;
    
    // Helpers
};
