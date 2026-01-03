#include "SmartHelper.h"
#include <iostream>
#include <fstream>
#include <windows.h>
#include <ntddscsi.h>
#include <vector>
#include <string>

// Definitions for modern storage queries
#ifndef IOCTL_STORAGE_QUERY_PROPERTY
#define IOCTL_STORAGE_QUERY_PROPERTY 0x002D1450
#endif

#ifndef IOCTL_STORAGE_PROTOCOL_COMMAND
#define IOCTL_STORAGE_PROTOCOL_COMMAND 0x002D1450
#endif

#pragma pack(push, 1)
struct NVME_HEALTH_LOG {
    UCHAR   CriticalWarning;
    USHORT  CompositeTemperature; // Kelvin
    UCHAR   Other[509]; 
};

struct MY_STORAGE_PROTOCOL_DATA_DESCRIPTOR {
    DWORD Version;
    DWORD Size;
    STORAGE_PROTOCOL_TYPE ProtocolType;
    DWORD ProtocolDataOffset;
    DWORD ProtocolDataLength;
    DWORD FixedProtocolReturnData;
    DWORD Reserved[3];
};

struct MY_STORAGE_PROTOCOL_SPECIFIC_DATA {
    STORAGE_PROTOCOL_TYPE ProtocolType;
    DWORD DataType;
    DWORD ProtocolDataRequestValue;
    DWORD ProtocolDataRequestSubValue;
    DWORD ProtocolDataOffset;
    DWORD ProtocolDataLength;
    DWORD FixedProtocolReturnData;
    DWORD Reserved[3];
};

struct MY_STORAGE_PROTOCOL_COMMAND {
    DWORD Version;
    DWORD Length;
    STORAGE_PROTOCOL_TYPE ProtocolType;
    DWORD Flags;
    DWORD ProtocolDataOffset;
    DWORD ProtocolDataLength;
    DWORD CommandSpecific;
    DWORD Reserved0;
    DWORD ProtocolSpecific;
    DWORD Reserved1;
    DWORD Reserved2[3];
};
#pragma pack(pop)

#ifndef ATA_FLAGS_DATA_IN
#define ATA_FLAGS_DATA_IN 0x02
#endif

SmartHelper::SmartHelper() {}
SmartHelper::~SmartHelper() {}

void SmartHelper::ScanDrives() {
    m_drives.clear();
    for (int i = 0; i < 4; ++i) { // Limit to 4 for speed
        std::string path = "\\\\.\\PhysicalDrive" + std::to_string(i);
        HANDLE hDevice = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice != INVALID_HANDLE_VALUE) {
            DriveInfo info;
            info.Index = i;
            info.Model = "Disk " + std::to_string(i); 
            
            STORAGE_PROPERTY_QUERY query = {};
            query.PropertyId = StorageDeviceProperty;
            query.QueryType = PropertyStandardQuery;
            char buffer[1024] = {};
            DWORD bytes = 0;
            if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &bytes, NULL)) {
                 STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
                 if (desc->ProductIdOffset > 0 && desc->ProductIdOffset < 1024) {
                     info.Model = std::string(buffer + desc->ProductIdOffset);
                     // Clean up trailing spaces
                     size_t last = info.Model.find_last_not_of(" ");
                     if (last != std::string::npos) info.Model.erase(last + 1);
                     
                     // If it's too short or generic, try Vendor too
                     if (info.Model.length() < 5 && desc->VendorIdOffset > 0 && desc->VendorIdOffset < 1024) {
                         std::string vendor = (char*)(buffer + desc->VendorIdOffset);
                         size_t vLast = vendor.find_last_not_of(" ");
                         if (vLast != std::string::npos) vendor.erase(vLast + 1);
                         info.Model = vendor + " " + info.Model;
                     }
                 }
            }
            info.Temperature = 0;
            m_drives.push_back(info);
            CloseHandle(hDevice);
        }
    }
}

void SmartHelper::UpdateTemps() {
    for (auto& drive : m_drives) {
        std::string path = "\\\\.\\PhysicalDrive" + std::to_string(drive.Index);
        HANDLE hDevice = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) continue;

        bool success = false;
        DWORD bytes = 0;

        // 1. Storage Query (Protocol Specific - NVMe SMART)
        // This is often the most reliable for Windows 10+
        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = (STORAGE_PROPERTY_ID)14; // StorageDeviceProtocolSpecificProperty
        query.QueryType = PropertyStandardQuery;
        
        #pragma pack(push, 1)
        struct {
            STORAGE_PROPERTY_QUERY Query;
            MY_STORAGE_PROTOCOL_SPECIFIC_DATA Protocol;
        } input = {};
        #pragma pack(pop)
        
        input.Query = query;
        input.Protocol.ProtocolType = ProtocolTypeNvme;
        input.Protocol.DataType = 2; // NVMeDataTypeLogPage
        input.Protocol.ProtocolDataRequestValue = 2; // SMART/Health Log
        input.Protocol.ProtocolDataOffset = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
        input.Protocol.ProtocolDataLength = 512;
        
        std::vector<BYTE> buffer(sizeof(MY_STORAGE_PROTOCOL_DATA_DESCRIPTOR) + 512, 0);
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &input, sizeof(input), buffer.data(), (DWORD)buffer.size(), &bytes, NULL)) {
            MY_STORAGE_PROTOCOL_DATA_DESCRIPTOR* desc = (MY_STORAGE_PROTOCOL_DATA_DESCRIPTOR*)buffer.data();
            if (desc->ProtocolDataLength >= sizeof(NVME_HEALTH_LOG)) {
                NVME_HEALTH_LOG* log = (NVME_HEALTH_LOG*)(buffer.data() + desc->ProtocolDataOffset);
                unsigned short k = log->CompositeTemperature;
                if (k > 1000) k = ((k & 0xFF) << 8) | (k >> 8);
                if (k >= 273 && k < 500) {
                    drive.Temperature = (float)(k - 273);
                    success = true;
                }
            }
        }

        // 2. Legacy Method Fallback (Property 8)
        if (!success) {
            query.PropertyId = (STORAGE_PROPERTY_ID)8; 
            char buf8[256];
            if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buf8, sizeof(buf8), &bytes, NULL)) {
                STORAGE_TEMPERATURE_DATA_DESCRIPTOR* desc = (STORAGE_TEMPERATURE_DATA_DESCRIPTOR*)buf8;
                if (desc->Size >= 20 && desc->InfoCount > 0) {
                    struct T_INFO { USHORT idx; SHORT temp; SHORT over; USHORT res; };
                    T_INFO* p = (T_INFO*)((BYTE*)buf8 + 24);
                    if (p->temp > 0 && p->temp < 150) { drive.Temperature = (float)p->temp; success = true; }
                }
            }
        }

        CloseHandle(hDevice);
    }
}
