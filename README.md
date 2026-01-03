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

### ⚙️ advanced Options
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

### Requirements
-   **HP OMEN** hardware (Laptop or Desktop).
-   **Windows 10/11**.
-   **Administrator Privileges** (Required to communicate with the OMEN Embedded Controller and HAL).

### Build from Source
The project uses **CMake** and **Dear ImGui** (DX11 backend).
1.  Clone the repository.
2.  Open with Visual Studio or build via CMake.
3.  Run `OmenReplace.exe` as Administrator.

---

## 🛡 Disclaimer
This tool interacts with low-level hardware settings (Undervolting, Overclocking, Fan Speeds). Use at your own risk. Incorrect settings can cause system instability or crashes. Always test new settings under load.
