#include <WiFi.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <math.h>
#include <WebServer.h>

// ============================================================================
// 1. CONFIGURATION MATÉRIELLE (BROCHES GPIO & OLED SSD1309)
// ============================================================================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* scl=*/ 26, /* sda=*/ 25);

const int TOUCH_PINS[4] = {13, 12, 14, 27};
bool lastTouchState[4] = {false, false, false, false};
unsigned long lastTouchDebounce = 0;
const unsigned long DEBOUNCE_DELAY = 140; // Réactivité accrue

const int LOCK_PIN = 32;
bool lastLockPinState = false;
unsigned long lockPressStartTime = 0;
bool longPressHandled = false;

// ============================================================================
// 2. CONFIGURATION SERVEUR WEB & POINT D'ACCÈS (SoftAP)
// ============================================================================
const char* AP_SSID = "ESP32_RF_Analyzer";
WebServer server(80);

// ============================================================================
// 3. STRUCTURES DE DONNÉES & ÉTATS DU SYSTÈME (8 MODES : 1A à 4B)
// ============================================================================
struct APInfo {
  String ssid;
  uint8_t bssid[6];
  String bssidStr;
  int32_t rssi;
  int32_t channel;
  wifi_auth_mode_t auth;
};

APInfo apList[20];
int apCount = 0;

int modeActif = 1; 
const char* modeLabels[9] = {"", "1A", "1B", "2A", "2B", "3A", "3B", "4A", "4B"};
const char* modeTitles[9] = {
  "", "INVENTAIRE RF", "AUDIT SECU & OUI", "SPECTRE 2.4GHz", "ROAMING MESH",
  "OSCILLO RSSI", "RADAR PROXIMITE", "MATRICE INTERF.", "QUALITE PHY/MCS"
};

bool lockActif = false;
uint8_t lockedBssid[6] = {0};
String lockedBssidStr = "AUTO";
int targetIndex = 0;

unsigned long popupEndTime = 0;
String popupText = "";

// Tampon circulaire 100 points pour l'Oscilloscope (Mode 3A)
int rssiHistory[100];
float oscilloSmooth = -75.0;
float currentJitter = 0.0;

// Variables Radar EMA & Log-Distance (Mode 3B)
float rssiLisse = -70.0;
float prevRssiLisse = -70.0;
bool emaInitialized = false;
float distanceEstimee = 1.0;
int radarTrend = 0;

// Variables Matrice d'interférence (Mode 4A)
int bilanCanaux[3] = {0, 0, 0};
int meilleurCanal = 1;

// Variables Roaming Mesh (Mode 2B)
int meshCount = 0;
int meshSecondIdx = -1;
int deltaHandoff = 0;
String roamStatus = "Borne Unique";

// Variables Qualité PHY, SNR & MCS (Mode 4B)
int noiseFloor = -96;
int currentSnr = 0;
int currentMcs = 0;
String currentMod = "BPSK 1/2";
int currentPhyRate = 7;
String currentSla = "CRITIQUE";

// Gestion non bloquante des timers pour fluidité maximale
bool scanEnCours = false;
unsigned long lastScanEnd = 0;
const unsigned long SCAN_PAUSE_MS = 1200; // Pause entre 2 scans pour fluidité Web/AP
unsigned long lastOscilloSample = 0;
unsigned long lastOledRefresh = 0;
bool oledNeedsUpdate = true;

// ============================================================================
// 4. INTERFACE WEB PRO (STYLE INSTRUMENTATION CLEAN & UNIFORME)
// ============================================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ELE3000 — Analyseur Spectral RF 2,4 GHz</title>
  <style>
    :root {
      --bg: #0b0f17;
      --surface: #131b2e;
      --surface-alt: #182238;
      --border: #23304c;
      --primary: #0ea5e9;
      --primary-dim: rgba(14, 165, 233, 0.14);
      --text: #f1f5f9;
      --muted: #8b9bb4;
      --ok: #10b981;
      --warn: #f59e0b;
      --danger: #ef4444;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      font-size: 13.5px;
      line-height: 1.45;
      padding: 18px 22px;
      max-width: 1440px;
      margin: 0 auto;
    }
    .mono { font-family: "JetBrains Mono", "Consolas", "SFMono-Regular", monospace; }

    /* TOPBAR */
    .topbar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      background: var(--surface);
      border: 1px solid var(--border);
      padding: 12px 18px;
      border-radius: 8px;
      margin-bottom: 14px;
    }
    .brand { display: flex; align-items: center; gap: 12px; }
    .dot-live {
      width: 9px; height: 9px; border-radius: 50%;
      background: var(--ok);
      box-shadow: 0 0 8px var(--ok);
    }
    .brand-title { font-size: 15px; font-weight: 700; letter-spacing: 0.3px; }
    .brand-sub { font-size: 12px; color: var(--muted); }
    .status-group { display: flex; gap: 8px; align-items: center; }
    .pill {
      padding: 5px 11px;
      border-radius: 5px;
      font-size: 11.5px;
      font-weight: 600;
      background: var(--surface-alt);
      border: 1px solid var(--border);
      color: var(--text);
    }
    .pill.accent { border-color: var(--primary); color: var(--primary); background: var(--primary-dim); }
    .pill.lock { border-color: var(--warn); color: var(--warn); background: rgba(245, 158, 11, 0.12); }

    /* NAVIGATION SEGMENTÉE */
    .toolbar {
      display: grid;
      grid-template-columns: repeat(8, 1fr) auto auto;
      gap: 6px;
      background: var(--surface);
      border: 1px solid var(--border);
      padding: 8px;
      border-radius: 8px;
      margin-bottom: 14px;
    }
    @media (max-width: 1050px) { .toolbar { grid-template-columns: repeat(4, 1fr); } }
    .btn {
      background: var(--surface-alt);
      color: var(--muted);
      border: 1px solid transparent;
      padding: 7px 10px;
      border-radius: 5px;
      font-size: 12px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.1s ease;
      text-align: center;
      white-space: nowrap;
    }
    .btn:hover { color: var(--text); border-color: var(--border); }
    .btn.active {
      background: var(--primary);
      color: #06131f;
      font-weight: 700;
    }
    .btn.action { background: #1e293b; color: var(--text); border-color: var(--border); }
    .btn.action:hover { border-color: var(--primary); color: var(--primary); }

    /* GRILLE KPI UNIFORME */
    .kpi-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 14px;
      margin-bottom: 14px;
    }
    @media (max-width: 980px) { .kpi-grid { grid-template-columns: 1fr; } }

    .card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 14px 16px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
    }
    .card-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 11.5px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.6px;
      color: var(--muted);
      padding-bottom: 10px;
      margin-bottom: 10px;
      border-bottom: 1px solid var(--border);
    }
    .kv-list { display: flex; flex-direction: column; gap: 7px; }
    .kv {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 13px;
    }
    .kv-label { color: var(--muted); }
    .kv-val { font-weight: 600; color: var(--text); }
    .highlight { color: var(--ok); font-size: 14px; font-weight: 700; }

    /* BARRES DE PROGRESSION */
    .ch-row { margin-bottom: 9px; }
    .ch-head { display: flex; justify-content: space-between; font-size: 12px; margin-bottom: 4px; }
    .track { width: 100%; height: 7px; background: #090d16; border-radius: 4px; overflow: hidden; border: 1px solid var(--border); }
    .fill { height: 100%; background: var(--primary); transition: width 0.25s ease; }

    /* GRILLE GRAPHIQUES */
    .charts-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 14px;
      margin-bottom: 14px;
    }
    @media (max-width: 980px) { .charts-grid { grid-template-columns: 1fr; } }
    canvas {
      width: 100%;
      height: 175px;
      background: #080c14;
      border-radius: 6px;
      border: 1px solid var(--border);
      display: block;
    }

    /* TABLEAU PROPRE */
    .table-wrap { overflow-x: auto; max-height: 290px; overflow-y: auto; }
    table { width: 100%; border-collapse: collapse; font-size: 12.5px; }
    th {
      position: sticky; top: 0;
      background: var(--surface-alt);
      color: var(--muted);
      text-transform: uppercase;
      font-size: 11px;
      letter-spacing: 0.5px;
      padding: 9px 12px;
      text-align: left;
      border-bottom: 1px solid var(--border);
    }
    td { padding: 8px 12px; border-bottom: 1px solid rgba(35, 48, 76, 0.5); }
    tr:hover { background: rgba(255, 255, 255, 0.02); }
    tr.selected { background: var(--primary-dim); }
    .btn-sm {
      background: transparent;
      border: 1px solid var(--border);
      color: var(--text);
      padding: 3px 9px;
      border-radius: 4px;
      font-size: 11px;
      cursor: pointer;
    }
    .btn-sm:hover { border-color: var(--primary); color: var(--primary); }
  </style>
</head>
<body>

  <div class="topbar">
    <div class="brand">
      <div class="dot-live" id="liveDot"></div>
      <div>
        <div class="brand-title">ANALYSEUR SPECTRAL & DIAGNOSTIC RF WI-FI 2,4 GHz</div>
        <div class="brand-sub">ELE3000 — Polytechnique Montréal &bull; Ralph Batanian (2299061)</div>
      </div>
    </div>
    <div class="status-group">
      <span class="pill accent mono" id="oledBadge">OLED : [1A] INVENTAIRE RF</span>
      <span class="pill mono" id="lockBadge">SUIVI : AUTO</span>
    </div>
  </div>

  <div class="toolbar">
    <button class="btn active" id="btn1" onclick="setMode(1)">1A &bull; Inventaire</button>
    <button class="btn" id="btn2" onclick="setMode(2)">1B &bull; Sécurité/OUI</button>
    <button class="btn" id="btn3" onclick="setMode(3)">2A &bull; Spectre 20MHz</button>
    <button class="btn" id="btn4" onclick="setMode(4)">2B &bull; Roaming Mesh</button>
    <button class="btn" id="btn5" onclick="setMode(5)">3A &bull; Oscilloscope</button>
    <button class="btn" id="btn6" onclick="setMode(6)">3B &bull; Radar Proximité</button>
    <button class="btn" id="btn7" onclick="setMode(7)">4A &bull; Interférences</button>
    <button class="btn" id="btn8" onclick="setMode(8)">4B &bull; Qualité PHY</button>
    <button class="btn action" onclick="sendAction('/lock?cycle=1')">Cible Suivante</button>
    <button class="btn action" onclick="sendAction('/lock?auto=1')">Mode AUTO</button>
  </div>

  <div class="kpi-grid">
    <div class="card">
      <div class="card-header"><span>Cible Isolée & Télémétrie Radar</span><span class="mono">MODES 1B / 3B</span></div>
      <div class="kv-list">
        <div class="kv"><span class="kv-label">SSID Cible</span><span class="kv-val" id="tSsid">—</span></div>
        <div class="kv"><span class="kv-label">Adresse BSSID</span><span class="kv-val mono" id="tBssid">—</span></div>
        <div class="kv"><span class="kv-label">Constructeur (OUI)</span><span class="kv-val" id="tOui">—</span></div>
        <div class="kv"><span class="kv-label">Sécurité & Canal</span><span class="kv-val mono" id="tAuth">—</span></div>
        <div class="kv"><span class="kv-label">Niveau RSSI (Brut / EMA)</span><span class="kv-val mono" id="tRssi">—</span></div>
        <div class="kv"><span class="kv-label">Distance Estimée</span><span class="highlight mono" id="tDist">—</span></div>
        <div class="kv"><span class="kv-label">Tendance Cinématique</span><span class="kv-val mono" id="tTrend">—</span></div>
      </div>
    </div>

    <div class="card">
      <div class="card-header"><span>Occupation Canaux Orthogonaux</span><span class="mono">MODE 4A</span></div>
      <div>
        <div class="ch-row">
          <div class="ch-head"><span>Canal 1 (2412 MHz)</span><span class="mono" id="c1Txt">0 %</span></div>
          <div class="track"><div class="fill" id="c1Bar" style="width:0%"></div></div>
        </div>
        <div class="ch-row">
          <div class="ch-head"><span>Canal 6 (2437 MHz)</span><span class="mono" id="c6Txt">0 %</span></div>
          <div class="track"><div class="fill" id="c6Bar" style="width:0%"></div></div>
        </div>
        <div class="ch-row">
          <div class="ch-head"><span>Canal 11 (2462 MHz)</span><span class="mono" id="c11Txt">0 %</span></div>
          <div class="track"><div class="fill" id="c11Bar" style="width:0%"></div></div>
        </div>
      </div>
      <div class="kv" style="padding-top:8px; border-top:1px solid var(--border);">
        <span class="kv-label">Recommandation Optimale</span>
        <span class="highlight mono" id="bestCh">CANAL 1</span>
      </div>
    </div>

    <div class="card">
      <div class="card-header"><span>Couche Physique (PHY) & Roaming</span><span class="mono">MODES 2B / 4B</span></div>
      <div class="kv-list">
        <div class="kv"><span class="kv-label">Plancher de Bruit (Est.)</span><span class="kv-val mono" id="pNoise">-96 dBm</span></div>
        <div class="kv"><span class="kv-label">Rapport Signal/Bruit (SNR)</span><span class="kv-val mono" id="pSnr">0 dB</span></div>
        <div class="kv"><span class="kv-label">Indice Modulation (802.11n)</span><span class="kv-val mono" id="pMcs">MCS 0</span></div>
        <div class="kv"><span class="kv-label">Constellation & Codage</span><span class="kv-val mono" id="pMod">BPSK 1/2</span></div>
        <div class="kv"><span class="kv-label">Débit Physique Brut</span><span class="highlight mono" id="pRate">7 Mbps</span></div>
        <div class="kv"><span class="kv-label">Qualité de Service (SLA)</span><span class="kv-val" id="pSla">—</span></div>
        <div class="kv"><span class="kv-label">Diagnostic Roaming Mesh</span><span class="kv-val mono" id="pRoam">—</span></div>
      </div>
    </div>
  </div>

  <div class="charts-grid">
    <div class="card">
      <div class="card-header"><span>Spectrogramme 2,4 GHz (Gabarits OFDM 20 MHz)</span><span class="mono">MODE 2A</span></div>
      <canvas id="specCanvas" width="640" height="190"></canvas>
    </div>
    <div class="card">
      <div class="card-header"><span>Oscilloscope Temporel RSSI (100 pts @ 4 Hz)</span><span class="mono" id="jitLabel">JITTER : 0.0 dB</span></div>
      <canvas id="oscCanvas" width="640" height="190"></canvas>
    </div>
  </div>

  <div class="card">
    <div class="card-header"><span>Inventaire RF Temps Réel (Cliquer sur « Isoler » pour verrouiller un BSSID)</span><span class="mono" id="apCountLbl">0 RÉSEAU</span></div>
    <div class="table-wrap">
      <table>
        <thead>
          <tr><th>État</th><th>SSID</th><th>BSSID (MAC)</th><th>Constructeur OUI</th><th>Canal</th><th>RSSI</th><th>Sécurité</th><th>Action</th></tr>
        </thead>
        <tbody id="apTable"></tbody>
      </table>
    </div>
  </div>

<script>
const modeNames = ["", "1A • INVENTAIRE RF", "1B • AUDIT SECU & OUI", "2A • SPECTRE 2.4GHz", "2B • ROAMING MESH", "3A • OSCILLO RSSI", "3B • RADAR PROXIMITE", "4A • MATRICE INTERF.", "4B • QUALITE PHY/MCS"];
let busy = false;

function highlightModeBtn(m) {
  for(let i=1; i<=8; i++) document.getElementById('btn'+i).className = (i === m) ? 'btn active' : 'btn';
  document.getElementById('oledBadge').innerText = 'OLED : [' + modeNames[m] + ']';
}

function setMode(m) {
  highlightModeBtn(m); // Réaction visuelle instantanée (0 ms de délai)
  fetch('/mode?m=' + m).then(r => r.json()).then(d => renderData(d)).catch(()=>{});
}

function sendAction(url) {
  fetch(url).then(r => r.json()).then(d => renderData(d)).catch(()=>{});
}

function lockMac(idx) {
  fetch('/lock?idx=' + idx).then(r => r.json()).then(d => renderData(d)).catch(()=>{});
}

function drawSpectrum(aps, targetBssid) {
  const c = document.getElementById('specCanvas');
  const ctx = c.getContext('2d');
  const w = c.width, h = c.height - 26;
  ctx.clearRect(0, 0, c.width, c.height);

  ctx.strokeStyle = '#182238'; ctx.fillStyle = '#64748b'; ctx.font = '11px Consolas, monospace';
  for(let db = -90; db <= -30; db += 20) {
    let y = h - ((db + 100) * (h - 14) / 70);
    ctx.beginPath(); ctx.moveTo(32, y); ctx.lineTo(w - 12, y); ctx.stroke();
    ctx.fillText(db + '', 4, y + 3);
  }
  for(let ch = 1; ch <= 13; ch++) {
    let x = 40 + (ch - 1) * ((w - 60) / 12);
    ctx.beginPath(); ctx.moveTo(x, 8); ctx.lineTo(x, h); ctx.stroke();
    ctx.fillText('C' + ch, x - 8, h + 16);
  }

  aps.forEach(ap => {
    let cx = 40 + (ap.ch - 1) * ((w - 60) / 12);
    let bw = ((w - 60) / 12) * 2.0;
    let peakY = h - Math.max(6, Math.min(h - 12, (ap.rssi + 100) * (h - 14) / 70));
    let isT = (ap.bssid === targetBssid);

    ctx.beginPath();
    ctx.moveTo(cx - bw, h);
    ctx.lineTo(cx - bw * 0.48, peakY);
    ctx.lineTo(cx + bw * 0.48, peakY);
    ctx.lineTo(cx + bw, h);
    ctx.strokeStyle = isT ? '#10b981' : '#0ea5e9';
    ctx.lineWidth = isT ? 2.2 : 1.2;
    ctx.stroke();
    ctx.fillStyle = isT ? 'rgba(16, 185, 129, 0.22)' : 'rgba(14, 165, 233, 0.08)';
    ctx.fill();
  });
}

function drawOscillo(hist, jitter) {
  const c = document.getElementById('oscCanvas');
  const ctx = c.getContext('2d');
  const w = c.width, h = c.height - 22;
  ctx.clearRect(0, 0, c.width, c.height);

  ctx.strokeStyle = '#182238'; ctx.fillStyle = '#64748b'; ctx.font = '11px Consolas, monospace';
  for(let db = -90; db <= -30; db += 20) {
    let y = h - ((db + 100) * (h - 12) / 70);
    ctx.beginPath(); ctx.moveTo(32, y); ctx.lineTo(w - 8, y); ctx.stroke();
    ctx.fillText(db + '', 4, y + 3);
  }

  ctx.beginPath();
  for(let i = 0; i < hist.length; i++) {
    let x = 34 + (i / 99) * (w - 44);
    let y = h - Math.max(4, Math.min(h - 8, (hist[i] + 100) * (h - 12) / 70));
    if(i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.strokeStyle = '#10b981'; ctx.lineWidth = 2; ctx.stroke();
  document.getElementById('jitLabel').innerText = 'JITTER : ' + jitter.toFixed(1) + ' dB';
}

function renderData(d) {
  highlightModeBtn(d.mode);
  const lb = document.getElementById('lockBadge');
  lb.innerText = d.locked ? ('LOCK : ' + d.lockMac) : 'SUIVI : AUTO (MAX RSSI)';
  lb.className = d.locked ? 'pill lock mono' : 'pill mono';

  document.getElementById('tSsid').innerText = d.tSsid;
  document.getElementById('tBssid').innerText = d.tBssid;
  document.getElementById('tOui').innerText = d.tOui;
  document.getElementById('tAuth').innerText = d.tAuth + '  (Canal ' + d.tCh + ')';
  document.getElementById('tRssi').innerText = d.tRssi + ' dBm  /  ' + d.rssiEma.toFixed(1) + ' dBm';
  document.getElementById('tDist').innerText = d.signalLost ? 'HORS PORTÉE' : ('~ ' + d.dist.toFixed(2) + ' m');
  document.getElementById('tTrend').innerText = d.trend;

  document.getElementById('c1Txt').innerText = d.c1 + ' %'; document.getElementById('c1Bar').style.width = d.c1 + '%';
  document.getElementById('c6Txt').innerText = d.c6 + ' %'; document.getElementById('c6Bar').style.width = d.c6 + '%';
  document.getElementById('c11Txt').innerText = d.c11 + ' %'; document.getElementById('c11Bar').style.width = d.c11 + '%';
  document.getElementById('bestCh').innerText = 'CANAL ' + d.bestCh;

  document.getElementById('pNoise').innerText = d.noise + ' dBm';
  document.getElementById('pSnr').innerText = d.snr + ' dB';
  document.getElementById('pMcs').innerText = 'MCS ' + d.mcs;
  document.getElementById('pMod').innerText = d.mod;
  document.getElementById('pRate').innerText = d.rate + ' Mbps';
  document.getElementById('pSla').innerText = d.sla;
  document.getElementById('pRoam').innerText = d.roam;

  document.getElementById('apCountLbl').innerText = d.aps.length + ' RÉSEAUX DÉTECTÉS';

  drawSpectrum(d.aps, d.tBssid);
  drawOscillo(d.hist, d.jitter);

  let rows = '';
  d.aps.forEach((ap, idx) => {
    let sel = (ap.bssid === d.tBssid);
    rows += `<tr class="${sel ? 'selected' : ''}">
      <td class="mono" style="color:${sel ? '#0ea5e9' : '#64748b'}; font-weight:700;">${sel ? '● CIBLE' : '○'}</td>
      <td style="font-weight:600;">${ap.ssid}</td>
      <td class="mono">${ap.bssid}</td>
      <td>${ap.oui}</td>
      <td class="mono">CH ${ap.ch}</td>
      <td class="mono" style="font-weight:700;">${ap.rssi} dBm</td>
      <td class="mono">${ap.auth}</td>
      <td><button class="btn-sm" onclick="lockMac(${idx})">${sel && d.locked ? 'Verrouillé' : 'Isoler'}</button></td>
    </tr>`;
  });
  document.getElementById('apTable').innerHTML = rows;
}

function pollData() {
  if (busy) return;
  busy = true;
  fetch('/data').then(r => r.json()).then(d => {
    renderData(d);
    busy = false;
  }).catch(() => { busy = false; });
}

setInterval(pollData, 400); // Rafraîchissement fluide à 2,5 Hz
window.onload = pollData;
</script>
</body>
</html>
)rawliteral";

// ============================================================================
// 5. FONCTIONS UTILITAIRES (OUI, CHIFFREMENT, TRONCATURE, POP-UP)
// ============================================================================
String tronquer(String str, int maxLen) {
  if (str.length() == 0) return "<Hidden>";
  if (str.length() > maxLen) return str.substring(0, maxLen);
  return str;
}

String formatBssid(uint8_t* b) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
  return String(buf);
}

String getAuthName(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP!";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "802.1X";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3";
    default: return "WPA2";
  }
}

String getVendorOUI(uint8_t* mac) {
  uint32_t oui = (mac[0] << 16) | (mac[1] << 8) | mac[2];
  switch (oui) {
    case 0x001A2B: case 0x004096: case 0x0024C4: case 0x58AC78: return "Cisco Sys.";
    case 0x000B86: case 0x24DE9A: case 0x94B40F: return "Aruba HP";
    case 0x24A43C: case 0x788A20: case 0xF09FC2: case 0x802AA8: return "Ubiquiti";
    case 0x50C7BF: case 0x14CC20: case 0xE8DE27: case 0xB0BE76: return "TP-Link";
    case 0xD46E0E: case 0x001D7E: case 0xC83A35: return "Arris/Helix";
    case 0x44E9DD: case 0x749D79: case 0xA06391: return "Sagem/Bell";
    default:
      if (mac[0] & 0x02) return "Mesh/Priv.";
      return "Std IEEE";
  }
}

void triggerPopup(String msg) {
  popupText = msg;
  popupEndTime = millis() + 950;
  oledNeedsUpdate = true;
}

// ============================================================================
// 6. ALGORITHMES MATHÉMATIQUES RF ("BOÎTES DE VERRE")
// ============================================================================
void executerCalculsRF() {
  targetIndex = -1;
  if (apCount > 0) {
    if (lockActif) {
      for (int i = 0; i < apCount; i++) {
        if (memcmp(apList[i].bssid, lockedBssid, 6) == 0) {
          targetIndex = i;
          break;
        }
      }
    } else {
      targetIndex = 0;
    }
  }

  int cibles[3] = {1, 6, 11};
  for (int k = 0; k < 3; k++) {
    int c = cibles[k];
    float score = 0;
    for (int i = 0; i < apCount; i++) {
      int dist = abs(apList[i].channel - c);
      if (dist <= 4) {
        long pNorm = map(constrain(apList[i].rssi, -95, -35), -95, -35, 2, 45);
        float recouvrement = (5.0 - dist) / 5.0;
        score += pNorm * recouvrement;
      }
    }
    bilanCanaux[k] = min(100, (int)round(score));
  }
  meilleurCanal = 1;
  int minScore = bilanCanaux[0];
  if (bilanCanaux[1] < minScore) { minScore = bilanCanaux[1]; meilleurCanal = 6; }
  if (bilanCanaux[2] < minScore) { minScore = bilanCanaux[2]; meilleurCanal = 11; }

  if (targetIndex >= 0 && targetIndex < apCount) {
    APInfo& cib = apList[targetIndex];

    if (!emaInitialized) {
      rssiLisse = cib.rssi;
      prevRssiLisse = rssiLisse;
      oscilloSmooth = cib.rssi;
      emaInitialized = true;
    } else {
      prevRssiLisse = rssiLisse;
      rssiLisse = 0.65f * (float)cib.rssi + 0.35f * rssiLisse;
    }
    float deltaRssi = rssiLisse - prevRssiLisse;
    if (deltaRssi > 1.2f) radarTrend = 1;
    else if (deltaRssi < -1.2f) radarTrend = -1;
    else radarTrend = 0;

    float exposant = (-40.0f - rssiLisse) / (10.0f * 2.7f);
    distanceEstimee = constrain(pow(10.0f, exposant), 0.3f, 99.9f);

    meshCount = 0;
    meshSecondIdx = -1;
    for (int i = 0; i < apCount; i++) {
      if (apList[i].ssid == cib.ssid && cib.ssid.length() > 0) {
        meshCount++;
        if (i != targetIndex && meshSecondIdx == -1) meshSecondIdx = i;
      }
    }
    if (meshCount > 1 && meshSecondIdx != -1) {
      deltaHandoff = abs(cib.rssi - apList[meshSecondIdx].rssi);
      if (apList[meshSecondIdx].channel == cib.channel) roamStatus = "Conflit Co-Canal!";
      else if (cib.rssi < -75 && deltaHandoff < 8) roamStatus = "Sticky Client!";
      else roamStatus = "Handoff Optimal";
    } else {
      deltaHandoff = 0;
      roamStatus = "Borne Unique";
    }

    int brouilleurs = 0;
    for (int i = 0; i < apCount; i++) {
      if (i != targetIndex && abs(apList[i].channel - cib.channel) <= 2) brouilleurs++;
    }
    noiseFloor = min(-78, -96 + 3 * brouilleurs);
    currentSnr = max(0, (int)cib.rssi - noiseFloor);

    if (currentSnr >= 32)      { currentMcs = 7; currentMod = "64-QAM 5/6"; currentPhyRate = 72; currentSla = "VoIP + 4K OK"; }
    else if (currentSnr >= 28) { currentMcs = 6; currentMod = "64-QAM 3/4"; currentPhyRate = 65; currentSla = "VoIP + HD OK"; }
    else if (currentSnr >= 24) { currentMcs = 5; currentMod = "64-QAM 2/3"; currentPhyRate = 58; currentSla = "Stream HD OK"; }
    else if (currentSnr >= 20) { currentMcs = 4; currentMod = "16-QAM 3/4"; currentPhyRate = 43; currentSla = "VoIP Stable"; }
    else if (currentSnr >= 16) { currentMcs = 3; currentMod = "16-QAM 1/2"; currentPhyRate = 29; currentSla = "Web Standard"; }
    else if (currentSnr >= 12) { currentMcs = 2; currentMod = "QPSK 3/4";   currentPhyRate = 21; currentSla = "Debit Reduit"; }
    else if (currentSnr >= 8)  { currentMcs = 1; currentMod = "QPSK 1/2";   currentPhyRate = 14; currentSla = "Lien Fragile"; }
    else                       { currentMcs = 0; currentMod = "BPSK 1/2";   currentPhyRate = 7;  currentSla = "SLA CRITIQUE"; }
  }
  oledNeedsUpdate = true;
}

// ============================================================================
// 7. GESTION DES ENTRÉES CAPACITIVES (TTP224 & TTP223)
// ============================================================================
void lireClavierModes() {
  for (int i = 0; i < 4; i++) {
    bool etat = digitalRead(TOUCH_PINS[i]);
    if (etat && !lastTouchState[i] && (millis() - lastTouchDebounce > DEBOUNCE_DELAY)) {
      lastTouchDebounce = millis();
      int modeA = (i * 2) + 1;
      int modeB = (i * 2) + 2;
      if (modeActif == modeA) modeActif = modeB;
      else modeActif = modeA;
      oledNeedsUpdate = true;
    }
    lastTouchState[i] = etat;
  }
}

void passerCibleSuivante() {
  if (apCount == 0) return;
  if (!lockActif) {
    lockActif = true;
    targetIndex = 0;
  } else {
    targetIndex = (targetIndex + 1) % apCount;
  }
  memcpy(lockedBssid, apList[targetIndex].bssid, 6);
  lockedBssidStr = apList[targetIndex].bssidStr;
  emaInitialized = false;
  executerCalculsRF();
  triggerPopup("LOCK: " + tronquer(apList[targetIndex].ssid, 12));
}

void activerModeAuto() {
  lockActif = false;
  lockedBssidStr = "AUTO";
  targetIndex = 0;
  emaInitialized = false;
  executerCalculsRF();
  triggerPopup("MODE: AUTO");
}

void lireTouchSensorWiFi() {
  bool etat = digitalRead(LOCK_PIN);
  if (etat && !lastLockPinState) {
    lockPressStartTime = millis();
    longPressHandled = false;
  } else if (etat && lastLockPinState) {
    if (!longPressHandled && (millis() - lockPressStartTime > 850)) {
      longPressHandled = true;
      activerModeAuto();
    }
  } else if (!etat && lastLockPinState) {
    if (!longPressHandled && (millis() - lockPressStartTime > 35)) {
      passerCibleSuivante();
    }
  }
  lastLockPinState = etat;
}

// ============================================================================
// 8. RENDU GRAPHIQUE OLED 128x64 (OPTIMISÉ SANS SURCHARGE I2C)
// ============================================================================
void dessinerEntete() {
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 0, 128, 10);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_5x7_tf);
  char head[32];
  snprintf(head, sizeof(head), "[%s] %s", modeLabels[modeActif], modeTitles[modeActif]);
  u8g2.drawStr(2, 8, head);
  if (lockActif) u8g2.drawStr(116, 8, "L");
  u8g2.setDrawColor(1);
}

void afficherOLED() {
  u8g2.clearBuffer();
  dessinerEntete();
  u8g2.setFont(u8g2_font_5x7_tf);

  if (apCount == 0) {
    u8g2.drawStr(14, 36, "Scan RF en cours...");
    u8g2.sendBuffer();
    return;
  }

  APInfo& cib = (targetIndex >= 0) ? apList[targetIndex] : apList[0];

  switch (modeActif) {
    case 1:
      for (int i = 0; i < min(apCount, 5); i++) {
        int y = 20 + i * 10;
        if (i == targetIndex) u8g2.drawStr(0, y, ">");
        u8g2.drawStr(7, y, tronquer(apList[i].ssid, 10).c_str());
        char info[16];
        snprintf(info, sizeof(info), "C%02d %ddB", apList[i].channel, apList[i].rssi);
        u8g2.drawStr(66, y, info);
      }
      break;

    case 2:
      u8g2.drawStr(2, 21, ("SSID: " + tronquer(cib.ssid, 16)).c_str());
      u8g2.drawStr(2, 32, ("MAC : " + cib.bssidStr).c_str());
      u8g2.drawStr(2, 43, ("OUI : " + getVendorOUI(cib.bssid)).c_str());
      u8g2.drawStr(2, 54, ("SECU: " + getAuthName(cib.auth) + " (Ch " + String(cib.channel) + ")").c_str());
      break;

    case 3:
      u8g2.drawLine(8, 52, 122, 52);
      u8g2.setFont(u8g2_font_4x6_tf);
      for (int c = 1; c <= 13; c += 2) {
        int x = 10 + (c - 1) * 9;
        char cs[4]; snprintf(cs, sizeof(cs), "%d", c);
        u8g2.drawStr(x - 2, 61, cs);
      }
      for (int i = 0; i < apCount; i++) {
        int cx = 10 + (apList[i].channel - 1) * 9;
        int h = map(constrain(apList[i].rssi, -95, -30), -95, -30, 4, 38);
        int topY = 52 - h;
        u8g2.drawLine(max(2, cx - 14), 52, max(2, cx - 6), topY);
        u8g2.drawLine(max(2, cx - 6), topY, min(125, cx + 6), topY);
        u8g2.drawLine(min(125, cx + 6), topY, min(125, cx + 14), 52);
      }
      break;

    case 4:
      u8g2.drawStr(2, 21, ("SSID: " + tronquer(cib.ssid, 16)).c_str());
      u8g2.drawStr(2, 32, ("Bornes Mesh : " + String(meshCount)).c_str());
      u8g2.drawStr(2, 43, ("Delta Handoff: " + String(deltaHandoff) + " dB").c_str());
      u8g2.drawStr(2, 56, roamStatus.c_str());
      break;

    case 5:
      u8g2.setFont(u8g2_font_4x6_tf);
      char oscStr[32];
      snprintf(oscStr, sizeof(oscStr), "%s | %ddBm Jit:%.1fdB", tronquer(cib.ssid, 9).c_str(), (int)round(oscilloSmooth), currentJitter);
      u8g2.drawStr(2, 18, oscStr);
      u8g2.drawFrame(14, 20, 102, 42);
      for (int i = 1; i < 100; i++) {
        int y1 = 60 - map(constrain(rssiHistory[i - 1], -100, -30), -100, -30, 1, 39);
        int y2 = 60 - map(constrain(rssiHistory[i], -100, -30), -100, -30, 1, 39);
        u8g2.drawLine(14 + i, y1, 15 + i, y2);
      }
      break;

    case 6:
      if (lockActif && targetIndex == -1) {
        u8g2.drawStr(6, 36, "! SIGNAL PERDU (Hors portee)");
      } else {
        u8g2.drawStr(2, 21, ("Cible: " + tronquer(cib.ssid, 14)).c_str());
        char dStr[28];
        snprintf(dStr, sizeof(dStr), "Dist: ~ %.1f m (%ddBm)", distanceEstimee, (int)round(rssiLisse));
        u8g2.drawStr(2, 33, dStr);
        int barW = map(constrain((int)rssiLisse, -95, -35), -95, -35, 4, 120);
        u8g2.drawFrame(2, 38, 124, 9);
        u8g2.drawBox(4, 40, barW, 5);
        if (radarTrend > 0) u8g2.drawStr(18, 59, ">>> EN APPROCHE >>>");
        else if (radarTrend < 0) u8g2.drawStr(18, 59, "<<< ELOIGNEMENT <<<");
        else u8g2.drawStr(12, 59, "=== DISTANCE STABLE ===");
      }
      break;

    case 7:
      {
        int chs[3] = {1, 6, 11};
        for (int k = 0; k < 3; k++) {
          int y = 21 + k * 11;
          char bStr[16];
          snprintf(bStr, sizeof(bStr), "CH%02d:%3d%%", chs[k], bilanCanaux[k]);
          u8g2.drawStr(2, y, bStr);
          u8g2.drawFrame(54, y - 6, 70, 7);
          u8g2.drawBox(55, y - 5, (bilanCanaux[k] * 68) / 100, 5);
        }
        char rec[28];
        snprintf(rec, sizeof(rec), "OPTIMAL: CANAL %d", meilleurCanal);
        u8g2.drawStr(16, 60, rec);
      }
      break;

    case 8:
      {
        char l1[28], l2[28], l3[28];
        snprintf(l1, sizeof(l1), "Bruit:%ddBm SNR:%ddB", noiseFloor, currentSnr);
        snprintf(l2, sizeof(l2), "MCS %d (%s)", currentMcs, currentMod.c_str());
        snprintf(l3, sizeof(l3), "Debit PHY: %d Mbps", currentPhyRate);
        u8g2.drawStr(2, 21, l1);
        u8g2.drawStr(2, 33, l2);
        u8g2.drawStr(2, 45, l3);
        u8g2.drawStr(2, 58, ("SLA: " + currentSla).c_str());
      }
      break;
  }

  if (millis() < popupEndTime) {
    u8g2.setDrawColor(0);
    u8g2.drawBox(10, 22, 108, 22);
    u8g2.setDrawColor(1);
    u8g2.drawFrame(10, 22, 108, 22);
    u8g2.drawStr(16, 36, popupText.c_str());
  }

  u8g2.sendBuffer();
}

// ============================================================================
// 9. RÉPONSE JSON UNIFIÉE & ROUTES WEB ULTRA-RAPIDES
// ============================================================================
void envoyerJsonEtat() {
  APInfo cib = (apCount > 0 && targetIndex >= 0) ? apList[targetIndex] : (apCount > 0 ? apList[0] : APInfo{"-", {0}, "-", -100, 1, WIFI_AUTH_OPEN});
  bool lost = (lockActif && targetIndex == -1);

  String json;
  json.reserve(2200); // Pré-allocation RAM pour éviter toute latence
  json = "{";
  json += "\"mode\":" + String(modeActif) + ",";
  json += "\"locked\":" + String(lockActif ? "true" : "false") + ",";
  json += "\"lockMac\":\"" + lockedBssidStr + "\",";
  json += "\"signalLost\":" + String(lost ? "true" : "false") + ",";
  json += "\"tSsid\":\"" + (cib.ssid.length() ? cib.ssid : "<Hidden>") + "\",";
  json += "\"tBssid\":\"" + cib.bssidStr + "\",";
  json += "\"tOui\":\"" + getVendorOUI(cib.bssid) + "\",";
  json += "\"tAuth\":\"" + getAuthName(cib.auth) + "\",";
  json += "\"tCh\":" + String(cib.channel) + ",";
  json += "\"tRssi\":" + String(cib.rssi) + ",";
  json += "\"rssiEma\":" + String(rssiLisse, 1) + ",";
  json += "\"dist\":" + String(distanceEstimee, 2) + ",";
  json += "\"trend\":\"" + String(radarTrend > 0 ? ">>> EN APPROCHE" : (radarTrend < 0 ? "<<< ELOIGNEMENT" : "=== STABLE ===")) + "\",";
  json += "\"c1\":" + String(bilanCanaux[0]) + ",\"c6\":" + String(bilanCanaux[1]) + ",\"c11\":" + String(bilanCanaux[2]) + ",";
  json += "\"bestCh\":" + String(meilleurCanal) + ",";
  json += "\"noise\":" + String(noiseFloor) + ",\"snr\":" + String(currentSnr) + ",";
  json += "\"mcs\":" + String(currentMcs) + ",\"mod\":\"" + currentMod + "\",";
  json += "\"rate\":" + String(currentPhyRate) + ",\"sla\":\"" + currentSla + "\",";
  json += "\"roam\":\"" + roamStatus + " (" + String(meshCount) + " AP, d=" + String(deltaHandoff) + "dB)\",";
  json += "\"jitter\":" + String(currentJitter, 2) + ",";

  json += "\"hist\":[";
  for (int i = 0; i < 100; i++) {
    json += String(rssiHistory[i]) + (i < 99 ? "," : "");
  }
  json += "],\"aps\":[";
  for (int i = 0; i < apCount; i++) {
    String cleanSsid = apList[i].ssid;
    cleanSsid.replace("\"", "");
    if (cleanSsid.length() == 0) cleanSsid = "<Hidden>";
    json += "{\"ssid\":\"" + cleanSsid + "\",\"bssid\":\"" + apList[i].bssidStr + "\",";
    json += "\"oui\":\"" + getVendorOUI(apList[i].bssid) + "\",\"ch\":" + String(apList[i].channel) + ",";
    json += "\"rssi\":" + String(apList[i].rssi) + ",\"auth\":\"" + getAuthName(apList[i].auth) + "\"}";
    if (i < apCount - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void configurerServeurWeb() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", index_html);
  });

  server.on("/mode", HTTP_GET, []() {
    if (server.hasArg("m")) {
      int m = server.arg("m").toInt();
      if (m >= 1 && m <= 8) {
        modeActif = m;
        oledNeedsUpdate = true;
        afficherOLED(); // Mise à jour immédiate de l'OLED dès le clic Web
      }
    }
    envoyerJsonEtat();
  });

  server.on("/lock", HTTP_GET, []() {
    if (server.hasArg("auto")) {
      activerModeAuto();
    } else if (server.hasArg("cycle")) {
      passerCibleSuivante();
    } else if (server.hasArg("idx")) {
      int idx = server.arg("idx").toInt();
      if (idx >= 0 && idx < apCount) {
        lockActif = true;
        targetIndex = idx;
        memcpy(lockedBssid, apList[idx].bssid, 6);
        lockedBssidStr = apList[idx].bssidStr;
        emaInitialized = false;
        executerCalculsRF();
        triggerPopup("WEB LOCK: " + tronquer(apList[idx].ssid, 10));
      }
    }
    afficherOLED();
    envoyerJsonEtat();
  });

  server.on("/data", HTTP_GET, []() {
    envoyerJsonEtat();
  });

  server.begin();
}

// ============================================================================
// 10. INITIALISATION (SETUP) ET BOUCLE TEMPS RÉEL ULTRA-FLUIDE (LOOP)
// ============================================================================
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(TOUCH_PINS[i], INPUT);
  pinMode(LOCK_PIN, INPUT);
  for (int i = 0; i < 100; i++) rssiHistory[i] = -85;

  Wire.begin(25, 26);
  Wire.setClock(400000); // Bus I2C Fast Mode (400 kHz) : transfert OLED 4x plus rapide !

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(10, 28, "ELE3000 - RF ANALYZER");
  u8g2.drawStr(10, 42, "AP: ESP32_RF_Analyzer");
  u8g2.drawStr(10, 54, "IP: 192.168.4.1");
  u8g2.sendBuffer();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, NULL, 1, 0, 4); // Canal 1 fixe, max 4 connexions
  Serial.print("Point d'acces actif. IP : ");
  Serial.println(WiFi.softAPIP());

  configurerServeurWeb();
  WiFi.scanNetworks(true, true);
  scanEnCours = true;
}

void loop() {
  // 1. Traitement prioritaire du serveur Web (latence < 2 ms)
  server.handleClient();

  // 2. Lecture immédiate des capteurs tactiles physiques
  lireClavierModes();
  lireTouchSensorWiFi();

  // 3. Gestion intelligente du scan Wi-Fi avec fenêtre de respiration pour le SoftAP
  if (scanEnCours) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      apCount = min(n, 20);
      for (int i = 0; i < apCount; i++) {
        apList[i].ssid = WiFi.SSID(i);
        memcpy(apList[i].bssid, WiFi.BSSID(i), 6);
        apList[i].bssidStr = formatBssid(apList[i].bssid);
        apList[i].rssi = WiFi.RSSI(i);
        apList[i].channel = WiFi.channel(i);
        apList[i].auth = WiFi.encryptionType(i);
      }
      WiFi.scanDelete();
      executerCalculsRF();
      scanEnCours = false;
      lastScanEnd = millis();
    } else if (n == WIFI_SCAN_FAILED) {
      scanEnCours = false;
      lastScanEnd = millis();
    }
  } else {
    // On attend 1,2 s entre deux scans pour que le Wi-Fi SoftAP et la page Web soient ultra-fluides
    if (millis() - lastScanEnd >= SCAN_PAUSE_MS) {
      WiFi.scanNetworks(true, true);
      scanEnCours = true;
    }
  }

  // 4. Échantillonnage fluide de l'oscilloscope temporel (4 Hz / 250 ms)
  if (millis() - lastOscilloSample >= 250) {
    lastOscilloSample = millis();
    int rawVal = (apCount > 0 && targetIndex >= 0) ? apList[targetIndex].rssi : -95;
    oscilloSmooth = 0.75f * (float)rawVal + 0.25f * oscilloSmooth;
    int val = (int)round(oscilloSmooth);

    float sum = 0, sumSq = 0;
    for (int i = 0; i < 99; i++) {
      rssiHistory[i] = rssiHistory[i + 1];
      sum += rssiHistory[i];
    }
    rssiHistory[99] = val;
    sum += val;
    float mean = sum / 100.0f;
    for (int i = 0; i < 100; i++) sumSq += pow(rssiHistory[i] - mean, 2);
    currentJitter = sqrt(sumSq / 100.0f);
    if (modeActif == 5) oledNeedsUpdate = true;
  }

  // 5. Rafraîchissement intelligent de l'OLED (uniquement si nécessaire ou fin de Pop-up)
  if (oledNeedsUpdate || (millis() - lastOledRefresh >= 200)) {
    lastOledRefresh = millis();
    oledNeedsUpdate = false;
    afficherOLED();
  }

  delay(5); // Boucle à 200 Hz pour une réactivité instantanée salut c fini
}