#pragma once
#include <string>

namespace OmenApi {

inline const std::string& GetDashboardHtml() {
  static const std::string html = []() {
    std::string s;
    s.reserve(16384);
    s += R"raw1(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>OMEN Control</title>
<style>
:root {
  --bg: #08080c;
  --card-bg: #0e0e14;
  --card-border: #23232c;
  --track-bg: #181820;
  --accent: #8b262a;
  --accent-hover: #a32e33;
  --text-main: #e6e6e6;
  --text-muted: #8e8e93;
  --green: #38a169;
  --amber: #e6c832;
  --red: #e63232;
  --bar-track: #26262e;
}
* { box-sizing: border-box; margin: 0; padding: 0; font-family: system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif; -webkit-tap-highlight-color: transparent; }
body { background: var(--bg); color: var(--text-main); display: flex; justify-content: center; padding: 12px 10px 30px; min-height: 100vh; }
.app-container { width: 100%; max-width: 440px; display: flex; flex-direction: column; gap: 10px; }

/* Header */
.header-bar { display: flex; justify-content: space-between; align-items: center; padding: 4px 6px; }
.power-stats { font-size: 13px; font-weight: 600; color: var(--text-main); letter-spacing: 0.3px; }
.hw-info { font-size: 11px; color: var(--text-muted); padding: 0 6px 2px; }
.status-pill { display: flex; align-items: center; gap: 6px; font-size: 11px; color: var(--text-muted); }
.status-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--amber); transition: background 0.3s ease; }
.status-dot.connected { background: var(--green); box-shadow: 0 0 6px rgba(56,161,105,0.6); }
.status-dot.error { background: var(--red); box-shadow: 0 0 6px rgba(230,50,50,0.6); }

/* Card */
.card { background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 12px; padding: 14px; display: flex; flex-direction: column; gap: 12px; }
.card-title { font-size: 13px; font-weight: 600; color: var(--text-main); text-transform: uppercase; letter-spacing: 0.5px; }

/* Telemetry Rows */
.telemetry-row { display: flex; flex-direction: column; gap: 5px; }
.metric-header { display: flex; justify-content: space-between; align-items: center; font-size: 13px; }
.metric-label { color: var(--text-main); font-weight: 500; }
.metric-val { font-weight: 600; font-variant-numeric: tabular-nums; }
.progress-track { height: 4px; background: var(--bar-track); border-radius: 2px; overflow: hidden; width: 100%; }
.progress-fill { height: 100%; background: var(--accent); width: 0%; transition: width 0.4s ease; border-radius: 2px; }

/* Disks */
.disk-row { display: flex; justify-content: space-between; align-items: center; font-size: 12px; padding: 2px 0; border-top: 1px solid rgba(255,255,255,0.04); padding-top: 6px; }
.disk-name { color: var(--text-muted); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 260px; }

/* Segmented Pills */
.control-group { display: flex; flex-direction: column; gap: 6px; }
.group-label { font-size: 12px; color: var(--text-muted); font-weight: 500; }
.pill-track { display: flex; background: var(--track-bg); border-radius: 8px; padding: 3px; gap: 2px; }
.pill-btn { flex: 1; border: none; background: transparent; color: var(--text-muted); font-size: 13px; font-weight: 600; padding: 9px 0; border-radius: 6px; cursor: pointer; transition: all 0.2s ease; min-height: 40px; }
.pill-btn:active { transform: scale(0.97); }
.pill-btn.active { background: var(--accent); color: #fff; box-shadow: 0 1px 3px rgba(0,0,0,0.3); }

/* AMD PBO Slider */
.slider-container { display: flex; flex-direction: column; gap: 8px; }
.slider-labels { display: flex; justify-content: space-between; font-size: 11px; color: var(--text-muted); font-weight: 600; }
.range-slider { -webkit-appearance: none; appearance: none; width: 100%; height: 6px; border-radius: 3px; background: var(--bar-track); outline: none; cursor: pointer; }
.range-slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 20px; height: 20px; border-radius: 50%; background: var(--accent); cursor: pointer; border: 2px solid var(--card-bg); box-shadow: 0 0 4px rgba(0,0,0,0.5); }
.range-slider::-moz-range-thumb { width: 20px; height: 20px; border-radius: 50%; background: var(--accent); cursor: pointer; border: 2px solid var(--card-bg); }
.slider-value-badge { text-align: center; font-size: 12px; color: var(--text-muted); font-weight: 500; }

/* Action Buttons & Toggles */
.toggle-row { display: flex; justify-content: space-between; align-items: center; min-height: 38px; cursor: pointer; }
.switch { position: relative; display: inline-block; width: 44px; height: 24px; }
.switch input { opacity: 0; width: 0; height: 0; }
.switch-slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: var(--bar-track); transition: .3s; border-radius: 24px; }
.switch-slider:before { position: absolute; content: ""; height: 18px; width: 18px; left: 3px; bottom: 3px; background-color: white; transition: .3s; border-radius: 50%; }
input:checked + .switch-slider { background-color: var(--accent); }
input:checked + .switch-slider:before { transform: translateX(20px); }

.action-btn { width: 100%; border: 1px solid var(--card-border); background: var(--track-bg); color: var(--text-main); font-size: 13px; font-weight: 600; padding: 12px; border-radius: 8px; cursor: pointer; transition: all 0.2s ease; display: flex; justify-content: center; align-items: center; gap: 8px; min-height: 44px; }
.action-btn:active { background: var(--accent); color: white; transform: scale(0.98); }

/* Modal for Token */
.modal-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.85); display: none; justify-content: center; align-items: center; padding: 20px; z-index: 999; }
.modal-overlay.active { display: flex; }
.modal-box { background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 14px; padding: 20px; width: 100%; max-width: 340px; display: flex; flex-direction: column; gap: 14px; box-shadow: 0 10px 30px rgba(0,0,0,0.8); }
.modal-box h3 { font-size: 15px; color: var(--text-main); font-weight: 600; }
.modal-box p { font-size: 12px; color: var(--text-muted); line-height: 1.4; }
.modal-box input { width: 100%; background: var(--track-bg); border: 1px solid var(--card-border); border-radius: 8px; padding: 10px; color: #fff; font-size: 14px; outline: none; }
.modal-box input:focus { border-color: var(--accent); }
.modal-box button { background: var(--accent); border: none; color: white; font-weight: 600; font-size: 13px; padding: 12px; border-radius: 8px; cursor: pointer; }
</style>
</head>
<body>

<div class="app-container">
  <!-- Top Bar -->
  <div class="header-bar">
    <div class="power-stats" id="power-header">C:--W | G:--W | --W</div>
    <div class="status-pill" id="token-btn" style="cursor:pointer;" title="Click to change API token">
      <div class="status-dot" id="status-dot"></div>
      <span id="status-text">Connecting...</span>
    </div>
  </div>
  <div class="hw-info" id="hw-header">OMEN Hardware Monitor</div>
)raw1";

    s += R"raw2(
  <!-- Card 1: Telemetry -->
  <div class="card">
    <div class="card-title">Telemetry</div>
    
    <div class="telemetry-row">
      <div class="metric-header">
        <span class="metric-label" id="cpu-label">CPU</span>
        <span class="metric-val" id="cpu-temp">--°C</span>
      </div>
      <div class="progress-track"><div class="progress-fill" id="cpu-load-bar"></div></div>
    </div>

    <div class="telemetry-row">
      <div class="metric-header">
        <span class="metric-label">GPU</span>
        <span class="metric-val" id="gpu-temp">--°C</span>
      </div>
      <div class="progress-track"><div class="progress-fill" id="gpu-load-bar"></div></div>
    </div>

    <div class="telemetry-row">
      <div class="metric-header">
        <span class="metric-label" id="ram-label">RAM</span>
        <span class="metric-val" id="ram-val">--</span>
      </div>
      <div class="progress-track"><div class="progress-fill" id="ram-load-bar"></div></div>
    </div>

    <div class="telemetry-row">
      <div class="metric-header">
        <span class="metric-label">Fans</span>
        <span class="metric-val" id="fans-val" style="color: var(--text-main);">-- / -- RPM</span>
      </div>
    </div>

    <div id="disks-container"></div>
  </div>

  <!-- Card 2: Power & Fan Controls -->
  <div class="card">
    <div class="control-group">
      <div class="group-label">Power Modes</div>
      <div class="pill-track" id="power-pills">
        <button class="pill-btn" onclick="setPowerMode(0)">Eco</button>
        <button class="pill-btn" onclick="setPowerMode(1)">Balanced</button>
        <button class="pill-btn" onclick="setPowerMode(2)">Perf</button>
      </div>
    </div>

    <div class="control-group">
      <div class="group-label">Fan Profiles</div>
      <div class="pill-track" id="fan-pills">
        <button class="pill-btn" onclick="setFanMode(0)">BIOS</button>
        <button class="pill-btn" onclick="setFanProfile(0)">Default</button>
        <button class="pill-btn" onclick="setFanProfile(1)">Quiet</button>
        <button class="pill-btn" onclick="setFanProfile(2)">Cool</button>
      </div>
    </div>

    <div class="control-group">
      <div class="group-label">GPU MUX</div>
      <div class="pill-track" id="mux-pills">
        <button class="pill-btn" onclick="setGpuMux(0)">Hybrid</button>
        <button class="pill-btn" onclick="setGpuMux(1)">Discrete</button>
      </div>
    </div>
  </div>

  <!-- Card 3: AMD PBO Undervolt -->
  <div class="card">
    <div class="card-title">AMD PBO Undervolt</div>
    <div class="slider-container">
      <div class="slider-labels">
        <span>-30</span>
        <span id="co-val-text">Set: 0 counts</span>
        <span>0</span>
      </div>
      <input type="range" min="-30" max="0" value="0" class="range-slider" id="co-slider" oninput="onCoSlide(this.value)" onchange="applyCo(this.value)">
    </div>
  </div>

  <!-- Card 4: System & Memory -->
  <div class="card">
    <div class="card-title">System Options</div>
    <label class="toggle-row">
      <span style="font-size: 13px; font-weight: 500;">80% Battery Care Mode</span>
      <div class="switch">
        <input type="checkbox" id="battery-chk" onchange="toggleBattery(this.checked)">
        <span class="switch-slider"></span>
      </div>
    </label>

    <button class="action-btn" id="flush-btn" onclick="flushRam()">
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12a9 9 0 0 0-9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/><path d="M3 12a9 9 0 0 0 9 9 9.75 9.75 0 0 0 6.74-2.74L21 16"/><path d="M16 21h5v-5"/></svg>
      <span>Flush RAM Cache</span>
    </button>
  </div>
</div>

<!-- Token Modal -->
<div class="modal-overlay" id="token-modal">
  <div class="modal-box">
    <h3>API Authentication</h3>
    <p>Enter the secret token configured in your laptop's <code>config.json</code>:</p>
    <input type="password" id="token-input" placeholder="Paste token here">
    <button onclick="saveToken()">Save & Connect</button>
  </div>
</div>
)raw2";

    s += R"raw3(
<script>
let token = localStorage.getItem('omen_api_token') || '';
let isUpdatingCo = false;

function getAuthHeader() {
  return token ? { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' } : { 'Content-Type': 'application/json' };
}

function checkToken() {
  if (!token) {
    document.getElementById('token-modal').classList.add('active');
  }
}
document.getElementById('token-btn').onclick = () => {
  document.getElementById('token-input').value = token;
  document.getElementById('token-modal').classList.add('active');
};
function saveToken() {
  token = document.getElementById('token-input').value.trim();
  localStorage.setItem('omen_api_token', token);
  document.getElementById('token-modal').classList.remove('active');
  fetchTelemetry();
}

function getTempColor(t) {
  if (t > 85) return 'var(--red)';
  if (t > 75) return 'var(--amber)';
  return 'var(--green)';
}

async function fetchTelemetry() {
  try {
    const res = await fetch('/api/telemetry', { headers: getAuthHeader() });
    if (res.status === 401) {
      document.getElementById('status-dot').className = 'status-dot error';
      document.getElementById('status-text').innerText = 'Unauthorized';
      document.getElementById('token-modal').classList.add('active');
      return;
    }
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const d = await res.json();
    
    document.getElementById('status-dot').className = 'status-dot connected';
    document.getElementById('status-text').innerText = 'Live';

    // Header
    document.getElementById('power-header').innerText = `C:${Math.round(d.cpu_power || 0)}W | G:${Math.round(d.gpu_power || 0)}W | ${Math.round(d.total_power || 0)}W`;
    if (d.cpu_name && d.gpu_name) {
      document.getElementById('hw-header').innerText = `${d.cpu_name} / ${d.gpu_name}`;
    }

    // CPU
    document.getElementById('cpu-label').innerText = `CPU (${(d.cpu_volts || 0).toFixed(2)}V)`;
    const cpuTempEl = document.getElementById('cpu-temp');
    cpuTempEl.innerText = `${Math.round(d.cpu_temp || 0)}°C`;
    cpuTempEl.style.color = getTempColor(d.cpu_temp || 0);
    document.getElementById('cpu-load-bar').style.width = `${Math.min(100, Math.max(0, d.cpu_load || 0))}%`;

    // GPU
    const gpuTempEl = document.getElementById('gpu-temp');
    gpuTempEl.innerText = `${Math.round(d.gpu_temp || 0)}°C`;
    gpuTempEl.style.color = getTempColor(d.gpu_temp || 0);
    document.getElementById('gpu-load-bar').style.width = `${Math.min(100, Math.max(0, d.gpu_load || 0))}%`;

    // RAM
    let ramTempStr = '';
    if (d.ram_dimms > 0 && d.ram_temp > 0 && d.ram_temp < 100) {
      if (d.ram_dimms >= 2) ramTempStr = `  ${Math.round(d.ram_temp0 || 0)}/${Math.round(d.ram_temp1 || 0)}°C`;
      else ramTempStr = `  ${Math.round(d.ram_temp || 0)}°C`;
    }
    const ramValEl = document.getElementById('ram-val');
    ramValEl.innerText = `${(d.ram_used_gb || 0).toFixed(1)}/${Math.round(d.ram_total_gb || 32)} GB${ramTempStr}`;
    ramValEl.style.color = d.ram_temp > 0 ? getTempColor(d.ram_temp) : 'var(--text-main)';
    document.getElementById('ram-load-bar').style.width = `${Math.min(100, Math.max(0, d.ram_pct || 0))}%`;

    // Fans
    const r1 = Math.round((d.fan1_rpm || 0) / 100) * 100;
    const r2 = Math.round((d.fan2_rpm || 0) / 100) * 100;
    document.getElementById('fans-val').innerText = `${r1} / ${r2} RPM`;

    // Disks
    if (d.drives && Array.isArray(d.drives)) {
      const cont = document.getElementById('disks-container');
      cont.innerHTML = '';
      d.drives.slice(0, 3).forEach(drv => {
        let model = drv.model.replace(/NVMe|NVME|PC/g, '').trim();
        const row = document.createElement('div');
        row.className = 'disk-row';
        row.innerHTML = `<span class="disk-name">${model} [${drv.health}%]</span><span class="metric-val" style="color:${getTempColor(drv.temp)}">${Math.round(drv.temp)}°C</span>`;
        cont.appendChild(row);
      });
    }

    // State sync
    if (d.state) {
      updateActivePill('power-pills', d.state.power_mode);
      const fanIdx = d.state.fan_mode === 0 ? 0 : (1 + (d.state.fan_profile || 0));
      updateActivePill('fan-pills', fanIdx);
      updateActivePill('mux-pills', d.state.gpu_mux || 0);
      document.getElementById('battery-chk').checked = (d.state.battery_limit || 100) <= 80;
      if (!isUpdatingCo) {
        document.getElementById('co-slider').value = d.state.amd_co || 0;
        document.getElementById('co-val-text').innerText = `Set: ${d.state.amd_co || 0} counts`;
      }
    }
  } catch (e) {
    document.getElementById('status-dot').className = 'status-dot error';
    document.getElementById('status-text').innerText = 'Offline';
  }
}

function updateActivePill(containerId, activeIdx) {
  const btns = document.getElementById(containerId).querySelectorAll('.pill-btn');
  btns.forEach((b, i) => {
    if (i === activeIdx) b.classList.add('active');
    else b.classList.remove('active');
  });
}

async function postControl(action, value) {
  try {
    await fetch('/api/control', {
      method: 'POST',
      headers: getAuthHeader(),
      body: JSON.stringify({ action: action, value: value })
    });
  } catch (e) {
    console.error('Failed to send control', e);
  }
}

function setPowerMode(mode) {
  updateActivePill('power-pills', mode);
  postControl('set_power_mode', mode);
}

function setFanMode(mode) {
  updateActivePill('fan-pills', 0);
  postControl('set_fan_mode', 0);
}

function setFanProfile(profile) {
  updateActivePill('fan-pills', profile + 1);
  postControl('set_fan_profile', profile);
}

function setGpuMux(mode) {
  updateActivePill('mux-pills', mode);
  postControl('set_gpu_mode', mode);
}

function toggleBattery(on) {
  postControl('set_battery_limit', on ? 80 : 100);
}

function onCoSlide(val) {
  isUpdatingCo = true;
  document.getElementById('co-val-text').innerText = `Set: ${val} counts`;
}

function applyCo(val) {
  isUpdatingCo = false;
  postControl('set_amd_co', parseInt(val, 10));
}

function flushRam() {
  const btn = document.getElementById('flush-btn');
  btn.innerText = 'Flushing...';
  postControl('flush_ram', true);
  setTimeout(() => {
    btn.innerHTML = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 6L9 17l-5-5"/></svg><span>Flushed!</span>`;
    setTimeout(() => {
      btn.innerHTML = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12a9 9 0 0 0-9-9 9.75 9.75 0 0 0 6.74-2.74L21 16"/><path d="M16 21h5v-5"/></svg><span>Flush RAM Cache</span>`;
    }, 1500);
  }, 500);
}

checkToken();
fetchTelemetry();
setInterval(fetchTelemetry, 1500);
</script>
</body>
</html>
)raw3";
    return s;
  }();
  return html;
}

} // namespace OmenApi
