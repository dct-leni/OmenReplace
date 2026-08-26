# OmenReplace (AMDOMEN Control)

**OmenReplace** is an ultra-lightweight, bloat-free, native C++ replacement for the HP OMEN Gaming Hub (OGH). It provides full hardware management—progressive fan curves, power limits, AMD Curve Optimizer undervolting, battery protection, hardware HUD overlay, and an embedded web dashboard—with **zero background telemetry**, instant startup, and a minimal memory footprint (<15 MB RAM vs 500+ MB for OGH).

---

## Table of Contents
- [User Guide](#user-guide)
  - [Key Features](#key-features)
  - [Prerequisites & Building](#prerequisites--building)
  - [Running & Usage](#running--usage)
  - [Configuration (`config.json`)](#configuration-configjson)
- [Technical Architecture & AI Knowledge Base](#technical-architecture--ai-knowledge-base)
  - [System Architecture Overview](#system-architecture-overview)
  - [Hardware Communication Layers](#hardware-communication-layers)
    - [1. Embedded Controller (EC) via PawnIO](#1-embedded-controller-ec-via-pawnio)
    - [2. HP BIOS WMI Interface (`hpqBIntM`)](#2-hp-bios-wmi-interface-hpqbintm)
    - [3. AMD SMU (System Management Unit)](#3-amd-smu-system-management-unit)
    - [4. Windows Power Overlay & Processor Settings](#4-windows-power-overlay--processor-settings)
    - [5. Native Task Scheduler 2.0 COM Subsystem](#5-native-task-scheduler-20-com-subsystem)
  - [Source Code Directory Structure](#source-code-directory-structure)
  - [Key Components & Symbol Reference](#key-components--symbol-reference)
  - [Embedded Web API & Mobile Dashboard](#embedded-web-api--mobile-dashboard)
  - [Build System & Toolchain Details](#build-system--toolchain-details)
  - [Modification & Extension Guide](#modification--extension-guide)

---

# User Guide

### Key Features

- **🚀 Native & Ultra-Fast**: Built purely in native C++20 and GDI/Win32. No Electron, no WebView2, no .NET runtime, no telemetry.
- **⚡ Power Modes & Hardware Power Limiting**:
  - **Eco**: 25W CPU PL1, CPU Boost Disabled (`GUID_PERFBOOST`), Energy Efficiency preference (EPP 80), Low GPU TGP, quiet acoustics.
  - **Balanced**: 45W CPU PL1, Efficient Boost (`GUID_PERFBOOST=3`), EPP 50, Standard GPU TGP.
  - **Performance**: 75W CPU PL1, Aggressive Boost, EPP 0, Max GPU TGP + Dynamic Boost (PPAB).
  - **Turbo / Max**: 254W CPU PL1 (uncapped), Max GPU TGP + PPAB.
- **❄️ Hybrid Progressive Fan Engine**:
  - **Piecewise Multi-Point Progressive Curves**: Replaces classical PID controllers with deterministic, calibrated thermal curves tailored for AMD Ryzen Dragon Range & Phoenix processors (`8940HX`).
  - **Profiles**:
    - **Default (Normal Mode)**: Whisper quiet idle (18% at ≤40°C), progressive smooth ramp (38% at 60°C, 70% at 80°C), scaling to 100% at 93°C.
    - **Quiet (Silence Priority)**: Keeps fans ≤45% up to 78°C for quiet office/browsing, only ramping if approaching thermal limits (100% at 95°C emergency).
    - **Cool (Max Performance)**: 30% baseline pre-cooling, ramping aggressively to 100% at 80°C to maximize CPU/GPU boost clocks and prevent thermal throttling.
  - **Instant Spike Protection**: Blends die temperature with smoothed average. If a sudden spike occurs ($T_{\text{cur}} > T_{\text{avg}} + 3^\circ\text{C}$), fan speed ramps immediately to arrest heat-soak.
  - **Asymmetric Slew-Rate Limiting**: Immediate ramp-up on heat spikes; smoothly capped **2.5%/sec ramp-down** to prevent fan hunting and acoustic revving.
  - **Hardware Delta Suppression ($\ge 2\%$)**: Hardware I/O writes (EC and WMI) are skipped if target speed hasn't changed by at least 2%, reducing ACPI bus traffic and background CPU load to **0.0%**.
- **🚀 Native Task Scheduler Autostart (COM `ITaskService`)**:
  - In-process registration using the official Windows Task Scheduler 2.0 COM API (`taskschd.h` / `taskschd.lib`). Starts elevated with `TASK_RUNLEVEL_HIGHEST` **without UAC prompts** at logon, with crash restart policy (10 retries, 1 min interval). Zero subprocess spawns, zero temporary scripts, sub-millisecond execution.
- **🛡️ Display Sleep & Taskbar Resilience**:
  - Listens for `RegisterWindowMessageW(L"TaskbarCreated")` to automatically recreate the system tray icon if Windows Explorer restarts or monitors wake from sleep.
  - Enforces a single instance via named mutex (`Local\AMDOMEN_SingleInstanceMutex`), automatically restoring the running window if launched again.
- **📉 AMD Curve Optimizer (PBO Undervolting)**:
  - Real-time all-core undervolting (-30 to +30 counts) via direct AMD SMU mailbox communication. Includes quick `[-]` / `[+]` stepping.
- **🌡️ CPU Temp Limit (Tctl)**:
  - MP1 SMU Tctl max presets (`Auto | 95° | 90° | 85°`) — lower limit = earlier boost cutback = cooler, quieter CPU. Persisted and re-applied (with verification) on startup.
- **🎮 GPU Power Override**:
  - `Auto | None | TGP | +Boost` pills (hover for explanation). `Auto` follows the Power Mode table; explicit values override it until cleared. Persisted and re-applied on startup.
- **🔋 Battery Care**:
  - 80% maximum charge limiter via HP WMI BIOS to preserve lithium-ion battery health.
- **🔌 Auto Power Switch**:
  - On battery: saves the current mode/profile and applies Eco + Quiet. On AC: restores what was saved.
- **📡 Wake-on-LAN Toggle**:
  - Reads the *real* NIC magic-packet state (`MSFT_NetAdapterPowerManagementSettingData`) and toggles it on all adapters.
- **🎮 Game Auto-Profile**:
  - Detects fullscreen foreground + GPU load > 40% sustained 5 s → switches to Performance + Cool fans; restores the previous state 10 s after the game exits.
- **🖥️ Hardware HUD Overlay**:
  - Sleek, semi-transparent obsidian HUD with CPU/GPU temps + loads and RAM usage (thin solid bars).
  - Magnetic screen border snapping, click-through capability, and topmost re-assertion while fullscreen apps are in the foreground.
- **🧩 Collapsible System Options**:
  - The System Options card collapses to a slim title strip (click the ▾/▴ chevron) to keep the window compact; starts compact each launch.
- **🌐 Embedded Web Dashboard & REST API**:
  - Built-in lightweight HTTP server with token authentication. Monitor hardware and switch power modes from a phone or web browser on your LAN.
- **🧹 Memory Working Set Flush**:
  - Instant one-click cache purge to free up unused RAM.

---

### Prerequisites & Building

- **Operating System**: Windows 10 or Windows 11 (x64)
- **Compiler**: Visual Studio 2022 / Build Tools (MSVC with C++20 support)
- **Build Tools**: CMake (>= 3.20) and Ninja
- **Driver**: [PawnIO](https://github.com/namazso/PawnIO) driver installed on the system (required for direct EC and SMU access).

#### Build Instructions
Run the automated build script from the project root:
```cmd
build.bat
```
The compiled executable is placed in `output\AMDOMEN.exe`.

---

### Running & Usage

> **Note**: `AMDOMEN.exe` requires **Administrator Privileges** to communicate with the PawnIO kernel driver and HP WMI BIOS interface.

- **Main Window**: 306x735 fixed-size obsidian dashboard with custom Fluent-style toggle switches and steppers.
- **System Tray**: Minimizing or closing the window places the application in the system tray. Right-click the tray icon to restore or cleanly exit.
- **Exit Teardown**: Exiting from the tray menu cleanly terminates the message loop, stops the API server, destroys the HUD, and restores automatic EC fan control.

---

### Configuration (`config.json`)

The application automatically loads and persists settings to `output\config.json`:

```json
{
  "power_mode": 1,
  "fan_mode": 0,
  "fan_profile": 0,
  "log_enabled": false,
  "tctl_limit": 90,
  "amd_curve_optimizer": -15,
  "battery_limit": 80,
  "minimize_on_close": true,
  "gpu_power_level": -1,
  "auto_power_switch": false,
  "game_auto_profile": false,
  "hud": {
    "show": true,
    "passthrough": true,
    "opacity": 0.8,
    "pos_x": 2480,
    "pos_y": 139,
    "size_w": 180,
    "size_h": 135
  },
  "api": {
    "enabled": true,
    "port": 8080,
    "bind_all": false,
    "token": "32-character-secret-token"
  }
}
```

**Key reference** (all root-level unless noted):

| Key | Values | Meaning |
|---|---|---|
| `power_mode` | 0–3 | Eco / Balanced / Performance / Turbo (re-applied with MP1 verification on start) |
| `fan_mode` | 0 / 2 | BIOS Auto / App control |
| `fan_profile` | 0–2 | Default / Quiet / Cool |
| `log_enabled` | bool | Root-level logger toggle (writes `amdomen.log` next to EXE; disabled by default) |
| `tctl_limit` | 0 / 75–105 | CPU temp limit (°C); 0 = Auto (firmware default) |
| `amd_curve_optimizer` | -30–30 | All-core Curve Optimizer counts |
| `battery_limit` | 80 / 100 | 80% charge limiter |
| `gpu_power_level` | -1–2 | GPU TGP override: -1 Auto / 0 None / 1 TGP / 2 +Boost |
| `auto_power_switch` | bool | Eco+Quiet on battery, restore on AC |
| `game_auto_profile` | bool | Perf+Cool while a fullscreen game is detected |
| `hud.*` | — | HUD visibility, position/size, opacity, click-through |
| `api.*` | — | REST server: enabled, port, bind_all (0.0.0.0), auth token |

---

# Technical Architecture & AI Knowledge Base

```
                                 +-----------------------------------+
                                 |         AMDOMEN.exe               |
                                 |   (Native Win32 C++20 Binary)     |
                                 +-----------------+-----------------+
                                                   |
           +---------------------------------------+---------------------------------------+
           |                                       |                                       |
+----------v-----------+               +-----------v-----------+               +-----------v-----------+
|    User Interfaces   |               |     HAL & Services    |               |  Embedded HTTP Server |
| - MainWindowWin32    |               | - OmenHal (Singleton) |               | - ApiServer (httplib) |
| - HudWindow (GDI)    |               | - PowerControl        |               | - Token Authorization |
| - TrayManager        |               | - FanService (Engine) |               | - Web Dashboard HTML  |
+----------------------+               +-----------+-----------+               +-----------------------+
                                                   |
        +------------------------------------------+------------------------------------------+
        |                                          |                                          |
+-------v---------------+                  +-------v---------------+                  +-------v---------------+
|  PawnIO Kernel Driver |                  |   HP WMI BIOS (WQL)   |                  | Windows Power & Tasks |
| - EC Port I/O (62/66) |                  | - hpqBIntM (0x20008)  |                  | - Power Overlay API   |
| - SMN / SMU Mailboxes |                  | - Method 0x1A (Mode)  |                  | - GUID_PERFBOOST/EPP  |
| - RAPL MSR Energy     |                  | - Method 0x29 (PL1/2) |                  | - ITaskService COM    |
| - SMBus DIMM Temp     |                  | - Method 0x22 (GPU)   |                  +-----------------------+
+-----------------------+                  +-----------------------+
```

---

## Hardware Communication Layers

### 1. Embedded Controller (EC) via PawnIO
Direct communication with the KBC / Embedded Controller using I/O ports `0x62` (Data) and `0x66` (Command/Status):
- **Mutex Sync**: `Global\Access_EC` cross-process named mutex protects against race conditions with other monitoring tools.
- **Key EC Registers**:
  - `0x2C` (`EC_XSS1`): Left Fan Target Duty Cycle (write).
  - `0x2D` (`EC_XSS2`): Right Fan Target Duty Cycle (write).
  - `0x2E` (`EC_XGS1`): Left Fan Current Duty Cycle (read).
  - `0x2F` (`EC_XGS2`): Right Fan Current Duty Cycle (read).
  - `0x57` (`EC_CPUT`): CPU Package Temperature (°C).
  - `0x58` (`EC_RTMP`): CPU Core Temperature (°C).
  - `0xB7` (`EC_GPU_TEMP`): GPU Temperature (°C).
  - `0x62` (`EC_OMCC`): Manual Fan Control Toggle (`0x06` = Manual, `0x00` = Auto).
  - `0x63` (`EC_XFCD`): Watchdog Heartbeat Timer (`0x1E` = 30 seconds).
  - `0xCE`: Hardware Power Mode selector (`0x00` = Balanced, `0x01` = Performance, `0x02` = Eco).

---

### 2. HP BIOS WMI Interface (`hpqBIntM`)
Communicates with HP BIOS ACPI methods in namespace `root\wmi`:
- **WMI Class**: `hpqBIntM`, Method: `hpqBIOSInt{N}` with signature `0x53, 0x45, 0x43, 0x55` (`SECU`).
- **Command Category `0x20008`**:
  - **Method `0x1A` (Thermal Profile)**: Sends `{ 0xFF, modeByte, 0x00, 0x00 }` (`0x30` = Balanced/Eco `L2`, `0x31` = Performance/Unleash `L7`).
  - **Method `0x29` (CPU Power Limits)**: Sends `{ PL2, PL1, PL4, TPP }`. Clamps wattage limits in hardware and EC regulators.
  - **Method `0x22` (GPU Power State & Dynamic Boost)**: Sends `{ GpuCustomTgp, GpuPpab, DState, Gps }` to control TGP ceiling and NVIDIA Dynamic Boost (PPAB).
  - **Method `0x24` (Battery Care Limit)**: Sends `{ 0x01 }` for 80% threshold, `{ 0x00 }` for 100%.
  - **Method `0x2E` (Fan Level)**: Direct WMI fan level setter.
  - **Method `0x11` (Fan Speed RPM)**: Queries fan tachometer RPM.
- **Command Category `0x00001`**:
  - **Method `0x52` (GPU MUX Switch)**: Queries / sets Advanced Optimus / Discrete GPU MUX state (`0` = Hybrid, `1` = Discrete, `2` = Integrated).

---

### 3. AMD SMU (System Management Unit)
Direct communication with the AMD SMU via System Management Network (SMN) using PCI configuration register aperture (`0xC4` / `0xC8`):
- **Mailbox Types**:
  - **RSMU (Root SMU)**: Mailbox for Curve Optimizer (CO).
    - `0x07` (`SMU_MSG_SetAllDldoPsmMargin`): Sets all-core CO offset.
    - `0xD5` (`SMU_MSG_GetDldoPsmMargin`): Reads active CO offset.
  - **MP1 (Power Management SMU)**: Mailbox for power envelope limits.
    - `0x3E` / `0x56`: `SMU_MSG_SetFastLimit` (Fast PPT in mW).
    - `0x5F` / `0xCB`: `SMU_MSG_SetSlowLimit` (Slow PPT in mW).
    - `0x4F`: `SMU_MSG_SetStapmLimit` (STAPM sustained limit in mW).
    - `0x3F`: `SMU_MSG_SetTctlMax` (Maximum thermal junction limit).
    - `0x23`: `SMU_MSG_GetSustainedPowerAndThmLimit` (Telemetry readback).

---

### 4. Windows Power Overlay & Processor Settings
Integrates with the Windows power scheme overlay engine (`powrprof.dll`):
- **Overlays**:
  - `GUID_OVERLAY_EFFICIENCY` (`961cc777-2547-4f9d-8174-7d86181b8a7a`)
  - `GUID_OVERLAY_BALANCED` (`00000000-0000-0000-0000-000000000000`)
  - `GUID_OVERLAY_PERFORMANCE` (`ded574b5-45a0-4f42-8737-46345c09c238`)
- **Processor Settings Subgroup (`54533251-82be-4824-96c1-47b60b740d00`)**:
  - `GUID_PERFBOOST` (`be337238-0d82-4146-a960-4f3749d470c7`): `0` = Disabled, `2` = Aggressive, `3` = Efficient Enabled.
  - `GUID_PERFEPP` (`36687f9e-e3a5-4dbf-b1dc-15eb381c6863`): Energy Performance Preference (`0` = Max Performance, `50` = Balanced, `80` = Power Saver).

---

### 5. Native Task Scheduler 2.0 COM Subsystem
Replaces legacy command-line process spawning (`schtasks.exe` / `powershell.exe`) with direct in-process COM API calls (`taskschd.h` / `taskschd.lib`):
- **`ITaskService`**: In-process connection to the Windows Task Scheduler service.
- **`IPrincipal`**: Configures `TASK_RUNLEVEL_HIGHEST` for silent elevation at user logon.
- **`ILogonTrigger`**: Triggers execution at user logon.
- **`ITaskSettings`**: Configures crash auto-restart interval (1 min), infinite execution limit, and unconstrained battery execution.
- **Performance**: Query execution < 1 ms, registration < 5 ms, 0 subprocesses, zero disk artifacts.

---

## Source Code Directory Structure

```
OmenReplace/
├── build.bat                    # Automated MSVC + Ninja build script
├── CMakeLists.txt               # CMake build definition (GLOB-based C++20)
├── output/
│   ├── AMDOMEN.exe              # Compiled standalone binary
│   └── config.json              # Active user & hardware configuration
├── src/
│   ├── main.cpp                 # WinMain entry, single-instance mutex, lifecycle
│   ├── gui/
│   │   ├── MainWindowWin32.h/.cpp # Native Win32 obsidian main window UI
│   │   ├── HudWindow.h/.cpp       # Semi-transparent GDI HUD overlay
│   │   └── TrayManager.h/.cpp     # System tray icon, TaskbarCreated listener, context menu
│   └── hal/
│       ├── OmenHal.h/.cpp         # Central hardware polling thread & cache
│       ├── OmenEc.h/.cpp          # Direct port I/O, SMU mailboxes, SMBus
│       ├── PawnIO.h/.cpp          # Low-level PawnIO driver interface
│       ├── PowerControl.h/.cpp    # HP WMI, AMD SMU PPT, Windows power plans
│       ├── FanService.h/.cpp      # Hybrid FanCurveEngine, profile curves, config JSON
│       ├── FanController.h/.cpp   # Serialized EC registers & WMI fan level hardware I/O
│       ├── WmiHelper.h/.cpp       # COM/WMI helper for HP BIOS methods
│       ├── ThermalService.h/.cpp  # Thermal sensor aggregation
│       ├── MemoryService.h/.cpp   # Working set & memory usage queries
│       ├── SmartHelper.h/.cpp     # NVMe / SSD SMART temperature queries
│       ├── ApiServer.h/.cpp       # Embedded HTTP server (httplib)
│       └── ApiDashboardHtml.h     # Bundled mobile-responsive web UI
└── vendor/
    ├── httplib.h                # cpp-httplib single-header server
    └── nlohmann/
        └── json.hpp             # Modern C++ JSON parser
```

---

## Key Components & Symbol Reference

| Component / Class | File | Responsibility |
|---|---|---|
| `OmenHal` | `src/hal/OmenHal.h` | Singleton hardware polling loop (1s interval). Caches CPU/GPU temperatures, fan RPMs, load percentages, and RAM statistics. Mutex-protected thread-safe accessors. |
| `OmenEc` | `src/hal/OmenEc.h` | Direct EC port `0x62`/`0x66` read/writes, watchdog heartbeat management, RSMU/MP1 SMU command execution via SMN. |
| `PowerControl` | `src/hal/PowerControl.h` | Power mode orchestration: coordinates WMI `0x1A`, WMI `0x29`, WMI `0x22`, AMD SMU PPT limits, and Windows overlays without race conditions. |
| `FanService` | `src/hal/FanService.h` | Manages fan modes (BIOS Auto, Default, Quiet, Cool) via `FanCurveEngine`, persists `config.json`, controls HUD visibility and battery limits. |
| `FanController` | `src/hal/FanController.h` | Single owner of hardware fan writes (EC registers + WMI fan level), serialized by mutex. |
| `MainWindowWin32` | `src/gui/MainWindowWin32.h` | Native Win32 UI (306x735). Custom GDI double-buffered rendering of cards, pill buttons, toggle switches, steppers, and native COM autostart management. |
| `HudWindow` | `src/gui/HudWindow.h` | Topmost layered window overlay. Double-buffered GDI rendering of CPU, GPU, and RAM telemetry with real-time border snapping in `WM_MOVING`. |
| `TrayManager` | `src/gui/TrayManager.h` | System tray icon manager with `TaskbarCreated` listener and instant window restore. |
| `ApiServer` | `src/hal/ApiServer.h` | Runs embedded HTTP REST server on configured port (default `8080`). Authenticates via Bearer token / `X-Api-Token`. Serves web dashboard and REST API. |

---

## Embedded Web API & Mobile Dashboard

The embedded HTTP server starts automatically with `AMDOMEN.exe` (if enabled in `config.json`).

### Web Dashboard
Navigate to `http://localhost:8080/` (or your LAN IP) in any desktop or mobile browser. It presents a dark-themed, mobile-friendly interface for monitoring thermals and controlling power/fan modes.

### REST API Endpoints
All API requests require authentication via `Authorization: Bearer <token>`, header `X-API-Token: <token>`, or `?token=<token>` query parameter.

| Endpoint | Method | Description |
|---|---|---|
| `/` | `GET` | Embedded web dashboard (token prompt in-browser) |
| `/api/telemetry` | `GET` | Full telemetry + current state as JSON. Add `?plain=1` for compact `key=value` text (ESP32-friendly) |
| `/api/state` | `GET` | Current modes only (power/fan/mux/battery/CO/GPU power/WoL) |
| `/api/control` | `POST` | Executes an action: `{"action": "<name>", "value": <int>}` |

**`/api/control` actions:**

| Action | Value | Description |
|---|---|---|
| `set_power_mode` | 0–2 | Eco / Balanced / Performance |
| `set_fan_mode` | 0 / 1 | BIOS Auto / App control |
| `set_fan_profile` | 0–2 | Default / Quiet / Cool (switches to App control) |
| `set_amd_co` | -30–0 | Curve Optimizer counts |
| `set_gpu_power` | -1–2 | TGP override: -1 Auto / 0 None / 1 TGP / 2 +Boost |
| `set_tctl_limit` | 0 / 75–105 | CPU temp limit; 0 = Auto |
| `set_wol` | 0 / 1 | Wake-on-LAN on all adapters |
| `set_battery_limit` | 80 / 100 | Battery charge threshold |
| `set_gpu_mode` | 0 / 1 | GPU MUX: Hybrid / Discrete (reboot required) |
| `flush_ram` | — | Purge working set memory cache |

---

## Build System & Toolchain Details

- **Language Standard**: C++20 (`-std:c++20` / `/std:c++20`)
- **Compilation Model**: GLOB-based CMake (`file(GLOB_RECURSE SRC "src/*.cpp")`). Adding any new `.cpp` file in `src/` compiles automatically without modifying `CMakeLists.txt`.
- **Linked Libraries**:
  - `user32.lib`, `gdi32.lib`, `shell32.lib`, `advapi32.lib` (Win32 OS APIs)
  - `ole32.lib`, `oleaut32.lib`, `wbemuuid.lib` (WMI / COM)
  - `taskschd.lib` (Windows Task Scheduler 2.0 COM API)
  - `powrprof.lib` (Windows Power Scheme & Overlay APIs)
  - `dwmapi.lib`, `msimg32.lib` (Desktop Window Manager & Alpha blending)
  - `ws2_32.lib` (Winsock for `httplib`)

---

## Modification & Extension Guide

### Adding a New Hardware Telemetry Metric
1. In `src/hal/OmenHal.h` / `OmenHal.cpp`, add the polling logic and thread-safe getter inside the `PollingLoop` thread.
2. In `src/hal/ApiServer.cpp`, include the metric inside the JSON response of `/api/telemetry`.
3. In `src/gui/HudWindow.cpp`, render the metric inside `OnPaint` (using the scaling factor `s = std::min(sx, sy)`).

### Adding a New HP WMI BIOS Command
1. In `src/hal/PowerControl.h`, declare the high-level method.
2. In `src/hal/PowerControl.cpp`, call `CallHpBios(0x20008, commandType, data, size, expectedOutSize)`.

### Adding a New SMU Register / Mailbox Message
1. In `src/hal/OmenEc.h`, declare the SMU accessor.
2. In `src/hal/OmenEc.cpp`, call `SendSmuCommand(msg, args)` for RSMU or `SendMp1Command(cmd, args)` for MP1 power management.
