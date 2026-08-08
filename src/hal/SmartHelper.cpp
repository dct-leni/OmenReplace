#include "SmartHelper.h"
#include "OmenLog.h"
#include <cctype>
#include <cstddef>
#include <iostream>
#include <fstream>
#include <windows.h>
#include <ntddscsi.h>
#include <ntddstor.h>
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
    UCHAR   CriticalWarning;      // offset 0
    USHORT  CompositeTemperature; // offset 1-2 (Kelvin)
    UCHAR   AvailableSpare;       // offset 3
    UCHAR   AvailableSpareThreshold; // offset 4
    UCHAR   PercentageUsed;      // offset 5 (0-100% wear used, NOT health)
    UCHAR   Other[506];          // offset 6..511 → total 512 bytes
};
#pragma pack(pop)

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

SmartHelper::SmartHelper() {}
SmartHelper::~SmartHelper() {}

void SmartHelper::ScanDrives() {
    m_drives.clear();
    for (int i = 0; i < 8; ++i) { // Scan more slots, filter below
        std::string path = "\\\\.\\PhysicalDrive" + std::to_string(i);
        HANDLE hDevice = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) continue;

        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        char buffer[1024] = {};
        DWORD bytes = 0;
        std::string model;
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &bytes, NULL)) {
             STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
             if (desc->ProductIdOffset > 0 && desc->ProductIdOffset < 1024) {
                 model = std::string(buffer + desc->ProductIdOffset);
                 size_t last = model.find_last_not_of(" ");
                 if (last != std::string::npos) model.erase(last + 1);
                 if (model.length() < 5 && desc->VendorIdOffset > 0 && desc->VendorIdOffset < 1024) {
                     std::string vendor = (char*)(buffer + desc->VendorIdOffset);
                     size_t vLast = vendor.find_last_not_of(" ");
                     if (vLast != std::string::npos) vendor.erase(vLast + 1);
                     model = vendor + " " + model;
                 }
             }
        }
        CloseHandle(hDevice);

        // Skip virtual/empty disks (no real model name or generic placeholder)
        bool virtualDisk = model.empty() || model.length() < 4;
        if (!virtualDisk) {
          std::string lower = model;
          for (auto &c : lower) c = (char)tolower((unsigned char)c);
          static const char *virtualPatterns[] = {
              "virtual", "vbox", "vmware", "msft", "microsoft", "ramdisk",
              "scsi disk", "file", "nvme cloud", "odx"};
          for (auto *p : virtualPatterns) {
            if (lower.find(p) != std::string::npos) {
              virtualDisk = true;
              break;
            }
          }
        }
        if (virtualDisk) continue;

        DriveInfo info;
        info.Index = i;
        info.Model = model;
        info.Temperature = 0;
        info.Health = 100;
        m_drives.push_back(info);
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
        // STORAGE_PROPERTY_QUERY = { PropertyId(4), QueryType(4),
        //   AdditionalParameters[1](1+3 pad) } = 12 bytes. The protocol data
        // must be written into AdditionalParameters, which starts at offset 8.
        // (The old code placed it at sizeof()=12, so the driver read garbage
        // at offset 8 → error 87 → SMART failed.)
        const size_t propOff = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
        BYTE input[sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) - 1] = {};
        STORAGE_PROPERTY_QUERY *pq = (STORAGE_PROPERTY_QUERY *)input;
        pq->PropertyId = StorageDeviceProtocolSpecificProperty;
        pq->QueryType = PropertyStandardQuery;

        STORAGE_PROTOCOL_SPECIFIC_DATA proto = {};
        proto.ProtocolType = ProtocolTypeNvme;
        proto.DataType = NVMeDataTypeLogPage;
        proto.ProtocolDataRequestValue = 2; // SMART/Health Log
        proto.ProtocolDataRequestSubValue = 0;
        proto.ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
        proto.ProtocolDataLength = 512;
        memcpy(input + propOff, &proto, sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA));

        BYTE buffer[sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) + 512] = {};
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, input, sizeof(input), buffer, sizeof(buffer), &bytes, NULL)) {
            STORAGE_PROTOCOL_DATA_DESCRIPTOR *desc = (STORAGE_PROTOCOL_DATA_DESCRIPTOR *)buffer;
            ULONG dataOff = desc->ProtocolSpecificData.ProtocolDataOffset;
            ULONG dataLen = desc->ProtocolSpecificData.ProtocolDataLength;
            OmenLog("[OMEN] disk%d %s nvme_proto_dataOff=%d dataLen=%d\n",
                    drive.Index, drive.Model.c_str(), (int)dataOff, (int)dataLen);
            if (dataLen >= sizeof(NVME_HEALTH_LOG) && dataOff > 0 &&
                dataOff + 8 + sizeof(NVME_HEALTH_LOG) <= sizeof(buffer)) {
                // NVMe log data has an 8-byte header before the health log.
                NVME_HEALTH_LOG *log = (NVME_HEALTH_LOG *)(buffer + dataOff + 8);
                // Dump first 16 bytes of the raw health log for debug.
                unsigned char *raw = (unsigned char *)log;
                OmenLog("[OMEN] disk%d %s nvme_log_raw: %02x %02x %02x %02x %02x %02x %02x %02x "
                        "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                        drive.Index, drive.Model.c_str(),
                        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                        raw[6], raw[7], raw[8], raw[9], raw[10], raw[11],
                        raw[12], raw[13], raw[14], raw[15]);
                unsigned short k = log->CompositeTemperature;
                OmenLog("[OMEN] disk%d %s nvme_composite_raw=0x%04x (%u) spare=%d used=%d\n",
                        drive.Index, drive.Model.c_str(), k, k,
                        log->AvailableSpare, log->PercentageUsed);
                // CompositeTemperature: 0x013B = 315 (whole Kelvin, 42°C) or
                // 0x0C4F = 3151 (0.1 K units, 42°C). Accept both, plus byte
                // swapped variants, and pick the interpretation in range.
                float tempC = 0.0f;
                const uint16_t variants[4] = {k, (uint16_t)((k >> 8) | (k << 8)),
                                              (uint16_t)(k / 10), k * 10};
                for (int vi = 0; vi < 4 && tempC == 0.0f; vi++) {
                    uint32_t v = variants[vi];
                    if (v >= 273 && v < 500) tempC = (float)v - 273.0f;
                    else if (v >= 2730 && v < 5000) tempC = (float)v / 10.0f - 273.0f;
                }
                if (tempC > 0 && tempC < 150) {
                    drive.Temperature = tempC;
                    success = true;
                }
                // Drive life = 100 - PercentageUsed (the % of the drive's
                // estimated life that has been consumed). AvailableSpare is
                // 100 on healthy drives, so it is NOT the life indicator.
                if (log->PercentageUsed > 0 && log->PercentageUsed <= 100)
                    drive.Health = 100 - (int)log->PercentageUsed;
            }
        }

        // 2. StorageDeviceTemperatureProperty (ID 17) fallback
        if (!success) {
            STORAGE_PROPERTY_QUERY tq = {};
            tq.PropertyId = StorageDeviceTemperatureProperty;
            tq.QueryType = PropertyStandardQuery;
            char buf[512] = {};
            if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &tq, sizeof(tq), buf, sizeof(buf), &bytes, NULL)) {
                STORAGE_TEMPERATURE_DATA_DESCRIPTOR *desc =
                    (STORAGE_TEMPERATURE_DATA_DESCRIPTOR *)buf;
                if (desc->Size >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) &&
                    desc->InfoCount > 0) {
                    // TemperatureInfo[] follows the fixed header (offset 24).
                    STORAGE_TEMPERATURE_INFO *p = desc->TemperatureInfo;
                    if (p->Temperature > 0 && p->Temperature < 150) {
                        drive.Temperature = (float)p->Temperature;
                        success = true;
                    }
                }
            }
        }

        CloseHandle(hDevice);
    }
}
