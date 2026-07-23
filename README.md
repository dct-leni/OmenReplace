# OmenReplace

**OmenReplace** is a lightweight, high-performance replacement for the OMEN gaming hub, designed for HP OMEN laptops and desktops. Built with C++ and ImGui, it provides granular control over hardware without the bloat of the original software.

> [!NOTE]
> This entire project, from core HAL logic to the visual interface, was **written by AI**.

---

## 🛠 Key Features

### 📊 Dashboard
The main control center for real-time monitoring and basic fan management.
-   **Monitoring**: Track CPU/GPU temperatures, real-time load, and total power draw (W).
-   **Drive Status**: Monitor NVMe/SSD temperatures to prevent thermal throttling.
-   **Cooling Control**: View current fan speeds in RPM and manually override speeds (0-100%) using intuitive sliders.
-   **Fan Modes**:
    -   **Auto**: Default BIOS-controlled fan curve.
    -   **Manual**: Fix fans at a specific percentage.
    -   **Sync**: Link CPU and GPU fan speeds for uniform cooling.
    -   **Optimized/Separated**: Use custom curves with independent or unified control.

### 📈 Fan Curve Editor
A fully interactive graph to design your own cooling profile.
-   **Visual Mapping**: Click and drag points on the curve to define fan behavior based on temperature.
-   **Independent Control**: Create separate curves for CPU and GPU fans or synchronize them with a single click.
-   **Real-time Feedback**: View a live indicator of where your hardware currently sits on the curve.

### ⚙️ Advanced Options
Deep hardware tuning for enthusiasts.
-   **Power Modes**: Quickly switch between **Eco**, **Balanced**, and **Turbo** profiles.
-   **GPU Mode Control**: Switch between **Hybrid**, **Discrete**, and **Integrated** GPU modes (requires reboot).
-   **CPU Undervolting**:
    -   **AMD**: Integrated Curve Optimizer (All-Core CO) for Ryzen chips.
    -   **Intel**: Core and Cache offset adjustments (mV) for 10th-13th Gen processors.
-   **GPU Overclocking (NVIDIA)**: Fine-tune Core/Memory clock offsets and adjust the Power Limit.
-   **Battery Care**: Set a custom charge limit (e.g., 60%, 80%) to prolong battery lifespan.
-   **System Integration**: Toggle "Run on Boot" and customize the HUD overlay.

### 🪄 Overlay HUD
A premium, transparent HUD that stays on top of your games.
-   **Minimalist Design**: Sleek, pixel-perfect layout with a "Lightsaber" glow bar for load visualization.
-   **Flexible Layout**: Toggle between horizontal and vertical orientations.
-   **Transparency Control**: Adjust opacity so it never distracts from the action.
-   **Context Menu**: Right-click directly on the HUD to toggle "Always on Top," "Resizable" mode, or layout options.

---

## 🚀 Getting Started

### Runtime Requirements
-   **HP OMEN** hardware (Laptop or Desktop)
-   **Windows 10/11**
-   **Administrator Privileges** — required to communicate with the OMEN Embedded Controller and HAL
-   **PawnIO** — kernel driver for low-level hardware access (EC/SMU communication).

#### How to Install PawnIO
PawnIO is required for this application to interface with the hardware. You must have `PawnIOLib.dll` and its driver.
1. Download PawnIO from its official source.
2. Install it so that `PawnIOLib.dll` is located at `C:\Program Files\PawnIO\PawnIOLib.dll`.
3. Alternatively, you can place `PawnIOLib.dll` directly in the `output` folder next to `OmenReplace.exe`.

---

## 🔨 Build from Source

### What is MSYS2 and MinGW-w64?
To build this project, you need a C++ compiler. We use **MinGW-w64 GCC**, which is a Windows port of the GNU Compiler Collection. 
**MSYS2** is simply a software distribution and building platform for Windows that provides a package manager (`pacman`) to easily download and install MinGW-w64 and other tools. It installs by default to `C:\msys64`.

> [!WARNING]
> **Should MSYS2 be added to the `vendor` folder?**
> **No.** MSYS2 and the MinGW compiler toolchain are massive (~1GB+) and vary per user machine. They are system-level build tools and should *never* be committed to a project's source code or `vendor` folder. The `vendor` folder is only for lightweight code dependencies like ImGui.

### Build Tool Requirements

| Tool | Purpose | Install |
|------|---------|---------|
| **CMake ≥ 3.14** | Build system | `winget install Kitware.CMake` |
| **MSYS2** | MinGW-w64 toolchain host | Place in `external_source\msys64` |
| **MinGW-w64 GCC ≥ 13** | C++20 compiler | Via MSYS2 (see below) |
| **mingw32-make** | Build runner | Via MSYS2 (see below) |

> [!IMPORTANT]
> The build script expects the **MSYS2 MinGW64** toolchain to be located in the project folder at `external_source\msys64\mingw64\bin`. Do **not** use Cygwin GCC or MSVC.

### MSYS2 Package Requirements

If setting up MSYS2 from scratch, download and extract/install MSYS2 into the `external_source\msys64` folder. Then open the **MSYS2 terminal** (`external_source\msys64\msys2.exe`) and run:

```bash
pacman -S --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-make
```

This single command installs everything the compiler needs:

| MSYS2 Package | Provides |
|---------------|---------|
| `mingw-w64-x86_64-gcc` | GCC 15 C++20 compiler + linker |
| `mingw-w64-x86_64-make` | `mingw32-make` build runner |
| *(auto-dependency)* `mingw-w64-x86_64-binutils` | `ld`, `objdump`, `objcopy` |
| *(auto-dependency)* `mingw-w64-x86_64-headers` | Windows SDK headers (d3d11, dxgi, wbemuuid, pdh, powrprof, etc.) |
| *(auto-dependency)* `mingw-w64-x86_64-crt` | MinGW C runtime |
| *(auto-dependency)* `mingw-w64-x86_64-winpthreads` | Static libpthread.a (baked into EXE at link time) |

### Bundled / No Installation Needed

| Dependency | Location | Notes |
|-----------|----------|-------|
| **Dear ImGui** | `vendor/imgui/` | Already in the repository |
| **DirectX 11** (`d3d11`, `dxgi`, `d3dcompiler`) | Provided by MinGW-w64 headers | No Windows SDK install needed |
| **WMI** (`wbemuuid`) | Provided by MinGW-w64 headers | — |
| **PDH / Power / PSApi** (`pdh`, `powrprof`, `psapi`) | Provided by MinGW-w64 headers | — |
| **DWM / IME / OLE** (`dwmapi`, `imm32`, `ole32`, `oleaut32`) | Provided by MinGW-w64 headers | — |

> [!NOTE]
> The resulting `OmenReplace.exe` is **fully standalone** — all MinGW runtime libraries (`libgcc`, `libstdc++`, `libwinpthread`) are statically baked in. No MSYS2 or MinGW required on the target machine.

### Building

Simply run `build.bat` from the project root:

```powershell
.\build.bat
```

The binary will be output to `output\OmenReplace.exe`.

Or build manually:

```powershell
$env:PATH = "$PWD\external_source\msys64\mingw64\bin;" + $env:PATH

cmake -G "MinGW Makefiles" `
  -DCMAKE_MAKE_PROGRAM="$PWD/external_source/msys64/mingw64/bin/mingw32-make.exe" `
  -DCMAKE_CXX_COMPILER="$PWD/external_source/msys64/mingw64/bin/g++.exe" `
  -B build

cmake --build build
```

### Running

Run `output\OmenReplace.exe` as **Administrator** (required for EC/HAL access).

---

## 🛡 Disclaimer
This tool interacts with low-level hardware settings (Undervolting, Overclocking, Fan Speeds). Use at your own risk. Incorrect settings can cause system instability or crashes. Always test new settings under load.
