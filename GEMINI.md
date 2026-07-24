# OMEN Control Optimizer — Hardware Abstraction & Architecture Reference

This document provides a comprehensive technical reference for the hardware abstraction layer (HAL), WMI BIOS methods, Embedded Controller (EC) register offsets, AMD SMU Mailbox commands, NVIDIA NVAPI/NVML integration, and UI component specifications for the HP OMEN 16 (ap-000... / Ryzen 9 8940HX / RTX 5070 Laptop) Control Application.

---

## 1. Hardware Abstraction Layer (HAL) Mapping

### A. HP WMI BIOS Interface (`root\wmi` -> `hpqBIntM`)
All HP WMI BIOS calls execute via `hpqBIOSInt` on instance `ACPI\PNP0C14\0_0`:

| Command / Category | Method Type ID | Data Payload Structure | Description / Function |
| :--- | :--- | :--- | :--- |
| **Battery Care Mode** | `BiosCmd.Default (0x20008)` / `0x24` | `data[0] = 0x01` (Enabled ~80%)<br>`data[0] = 0x00` (Disabled 100%) | Limits battery charge to ~80% to preserve lithium longevity. **Must pass `0x01` for enabled, not raw `80`**. |
| **Thermal / Power Mode** | `BiosCmd.Default (0x20008)` / `0x1A` | `data[0] = 0xFF`, `data[1] = Mode` | Switches thermal profile (`0x30`=Balanced, `0x50`=Cool, `0x31`=Performance). |
| **WMI Fan Level Write** | `BiosCmd.Default (0x20008)` / `0x2E` | `data[0] = CpuRpm`, `data[1] = GpuRpm` | Direct WMI fan speed override (scaled 0-55 where 55 = ~5500 RPM). |
| **WMI Fan Heartbeat** | `BiosCmd.Default (0x20008)` / `0x31` | `data[0] = 0x1E` (30s watchdog) | Resets BIOS manual fan watchdog timer. |
| **Max Fan Toggle** | `BiosCmd.Default (0x20008)` / `0x27` | `data[0] = 0x01` (Max), `0x00` (Auto) | Forces 100% max fan turbo boost. |
| **GPU Mode Switch** | `BiosCmd.GpuMode (0x00002)` / `0x52` | `data[0] = Mode` | `0`=Hybrid (Optimus), `1`=Discrete (MUX Direct), `2`=Integrated (iGPU). Requires reboot. |

---

### B. Embedded Controller (EC) Register Map (`OmenEc.cpp`)
EC registers are accessed via direct LPC port I/O (`0x62`/`0x66` ports) or `PawnIO` kernel driver:

| Offset | Symbol Name | Access | Value / Description |
| :--- | :--- | :--- | :--- |
| `0xCE` | `EC_POWER_MODE` | R/W | `0x00`/`0x30` = Balanced, `0x01`/`0x31` = Performance/Turbo, `0x02`/`0x50` = Eco/Cool |
| `0xF4` | `EC_XSS1` | Write | CPU Fan manual target percentage (0 - 100%) |
| `0xF5` | `EC_XSS2` | Write | GPU Fan manual target percentage (0 - 100%) |
| `0x34` | `EC_KRPM1` | Write | CPU Fan target KRPM (0 - 55) |
| `0x35` | `EC_KRPM2` | Write | GPU Fan target KRPM (0 - 55) |
| `0x2C` | `EC_RPM1_L` / `H` | Read | CPU Fan 1 current speed RPM |
| `0x2E` | `EC_RPM2_L` / `H` | Read | GPU Fan 2 current speed RPM |
| `0x57` | `EC_CPUT` | Read | CPU Package Temperature (°C) |
| `0x58` | `EC_RTMP` | Read | CPU Core Temperature (°C) |
| `0x5B` | `EC_GPU_TEMP` | Read | GPU Core Temperature (°C) |
| `0xE4` | `EC_OMCC` | Write | EC Manual Control Enable (`0x06` = Manual Lock, `0x00` = Auto) |
| `0xE5` | `EC_XFCD` | Write | EC Heartbeat Watchdog (`0x1E` = 30-second timer) |

---

## 2. Telemetry & User Interface Guidelines

### Original HUD Overlay Design (`OmenReplace-master`)
- **Visual Style**: Restored exact lightsaber segmented progress bar widget from `external_source\OmenReplace-master`.
- **Text & Colors**: Tight `CPU` / `GPU` label + temperature text in neon teal (`IM_COL32(50, 240, 180, 255)`).
- **Segmented Bar**: 10-segment track below text with solid white core (`255, 255, 255, 255`) + bloom overlay (`255, 255, 255, 45`).
- **Scalability**: Scaling factor slider (`0.8x` to `1.5x`) and height adjustment.
- **Pass-Through Mode**: Set `WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE` when Click-Through is enabled so in-game clicks pass directly to the game.

---

### System Power Wattage Color Rules
- **`< 60 W`**: Green (`0.20f, 0.90f, 0.52f, 1.0f`)
- **`60 W – 150 W`**: Yellow (`0.95f, 0.65f, 0.08f, 1.0f`)
- **`> 150 W`**: Red (`0.90f, 0.20f, 0.20f, 1.0f`)

---

### Fan Control Sliders & Graph Truth
- **Manual Sliders**: Specify desired target fan percentage (0-100%).
- **Graph Canvas**: Ground truth displaying **REAL ACTUAL fan speed** (`hal.GetFanSpeed(0)` / RPM) and real temperature (`hal.GetCpuTemp()`), NOT locked to static line points.
