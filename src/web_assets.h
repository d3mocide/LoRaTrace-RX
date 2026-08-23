#pragma once
// LoRaTrace RX — the web UI's entire frontend, embedded in flash.
//
// One self-contained HTML file (inline CSS + vanilla JS, no framework, no
// external assets) rather than a LittleFS/SPIFFS partition: the AP has no
// internet access anyway, so nothing external could load even if this
// pulled in a CDN. Served whole via WebServer::send_P() straight from
// flash — see wifi_task.cpp. PROGMEM is close to a no-op on ESP32 (flash is
// memory-mapped), kept for the same reason the rest of this codebase favors
// explicit-over-implicit: it says "this lives in flash, not RAM" at the
// point of use rather than relying on the compiler's default placement.

#include <pgmspace.h>

const char INDEX_HTML[] PROGMEM = R"WEBPAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LoRaTrace RX</title>
<style>
  :root {
    --bg: #0b0f14;
    --surface: #131a22;
    --surface-2: #1b232d;
    --border: #263140;
    --text: #e6edf3;
    --text-dim: #8b98a5;
    --accent: #38bdf8;
    --good: #4ade80;
    --warn: #facc15;
    --bad: #f87171;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
  }
  header {
    padding: 16px 20px 0;
    border-bottom: 1px solid var(--border);
  }
  header h1 {
    margin: 0 0 12px;
    font-size: 18px;
    font-weight: 600;
    letter-spacing: 0.02em;
  }
  header h1 span { color: var(--accent); }
  nav {
    display: flex;
    gap: 4px;
  }
  nav button {
    background: none;
    border: none;
    color: var(--text-dim);
    padding: 10px 16px;
    font-size: 14px;
    cursor: pointer;
    border-bottom: 2px solid transparent;
  }
  nav button.active {
    color: var(--text);
    border-bottom-color: var(--accent);
  }
  main {
    padding: 20px;
    max-width: 720px;
    margin: 0 auto;
  }
  .tab { display: none; }
  .tab.active { display: block; }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
    gap: 10px;
  }
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 12px 14px;
  }
  .card .label {
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--text-dim);
    margin-bottom: 4px;
  }
  .card .value {
    font-size: 20px;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .card .value.good { color: var(--good); }
  .card .value.warn { color: var(--warn); }
  .card .value.bad { color: var(--bad); }
  .section-title {
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-dim);
    margin: 22px 0 10px;
  }
  .section-title:first-child { margin-top: 0; }
  form {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 16px;
    display: grid;
    gap: 12px;
  }
  label {
    display: block;
    font-size: 12px;
    color: var(--text-dim);
    margin-bottom: 4px;
  }
  input {
    width: 100%;
    background: var(--surface-2);
    border: 1px solid var(--border);
    border-radius: 6px;
    color: var(--text);
    padding: 8px 10px;
    font-size: 14px;
    font-variant-numeric: tabular-nums;
  }
  input:focus { outline: 1px solid var(--accent); }
  button.primary {
    background: var(--accent);
    color: #04202e;
    border: none;
    border-radius: 6px;
    padding: 10px 16px;
    font-size: 14px;
    font-weight: 600;
    cursor: pointer;
    justify-self: start;
  }
  .note {
    font-size: 12px;
    color: var(--text-dim);
    margin-top: 4px;
  }
  #configMsg { font-size: 13px; min-height: 18px; }
  #configMsg.ok { color: var(--good); }
  #configMsg.err { color: var(--bad); }
  .runlist { display: grid; gap: 8px; }
  .run {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 10px;
    padding: 12px 14px;
    display: flex;
    align-items: center;
    justify-content: space-between;
  }
  .run .name { font-weight: 600; font-variant-numeric: tabular-nums; }
  .run .links a {
    color: var(--accent);
    text-decoration: none;
    font-size: 13px;
    margin-left: 14px;
  }
  .run .links a:hover { text-decoration: underline; }
  .empty { color: var(--text-dim); font-size: 14px; padding: 20px 0; }
</style>
</head>
<body>
<header>
  <h1>LoRa<span>Trace</span> RX</h1>
  <nav>
    <button class="tab-btn active" data-tab="status">Status</button>
    <button class="tab-btn" data-tab="downloads">Downloads</button>
    <button class="tab-btn" data-tab="settings">Settings</button>
  </nav>
</header>
<main>

  <section id="tab-status" class="tab active">
    <div class="section-title">Radio</div>
    <div class="grid" id="radioStats"></div>
    <div class="section-title">Logger / SD</div>
    <div class="grid" id="loggerStats"></div>
    <div class="section-title">GPS</div>
    <div class="grid" id="gpsStats"></div>
    <div class="section-title">System</div>
    <div class="grid" id="systemStats"></div>
  </section>

  <section id="tab-downloads" class="tab">
    <div class="section-title">Runs on this card</div>
    <div class="runlist" id="runList"><div class="empty">Loading…</div></div>
  </section>

  <section id="tab-settings" class="tab">
    <div class="section-title">Active LoRa channel</div>
    <form id="configForm">
      <div>
        <label for="freq_mhz">Frequency (MHz)</label>
        <input id="freq_mhz" name="freq_mhz" type="number" step="0.001" min="868" max="928">
      </div>
      <div>
        <label for="sf">Spreading factor (SF5–SF12)</label>
        <input id="sf" name="sf" type="number" min="5" max="12">
      </div>
      <div>
        <label for="bw_khz">Bandwidth (kHz)</label>
        <input id="bw_khz" name="bw_khz" type="number" step="0.1" min="0.1">
      </div>
      <div>
        <label for="cr_denom">Coding rate denominator (4/5–4/8)</label>
        <input id="cr_denom" name="cr_denom" type="number" min="5" max="8">
      </div>
      <div>
        <label for="sync_word">Sync word (hex, e.g. 0x2B)</label>
        <input id="sync_word" name="sync_word" type="text">
      </div>
      <div>
        <button class="primary" type="submit">Save</button>
        <div id="configMsg"></div>
      </div>
      <div class="note">Saved to /loratrace/config.txt — takes effect on next boot, same as editing the file by hand. The running radio is not touched.</div>
    </form>
  </section>

</main>
<script>
function statCard(label, value, cls) {
  return '<div class="card"><div class="label">' + label + '</div><div class="value' +
    (cls ? ' ' + cls : '') + '">' + value + '</div></div>';
}

function refreshStatus() {
  fetch('/api/status').then(r => r.json()).then(function (s) {
    var dropCls = (s.queue_drop + s.row_drop + s.bus_miss) === 0 ? 'good' : 'bad';
    document.getElementById('radioStats').innerHTML =
      statCard('Packets', s.rx) +
      statCard('CRC errors', s.crc_err) +
      statCard('Queue drops', s.queue_drop, dropCls) +
      statCard('Bus misses', s.bus_miss, dropCls);
    document.getElementById('loggerStats').innerHTML =
      statCard('Rows written', s.rows) +
      statCard('Rows dropped', s.row_drop, s.row_drop === 0 ? 'good' : 'bad') +
      statCard('SD', s.sd_ready ? 'ok' : 'DOWN', s.sd_ready ? 'good' : 'bad') +
      statCard('Run', s.run) +
      statCard('Max flush', s.max_flush_ms + 'ms') +
      statCard('Max health', s.max_session_ms + 'ms');
    document.getElementById('gpsStats').innerHTML =
      statCard('Fix', s.has_fix ? (s.lat.toFixed(5) + ', ' + s.lon.toFixed(5)) : 'none',
                s.has_fix ? 'good' : 'warn') +
      statCard('Sats used', s.sats) +
      statCard('Sats in view', s.sats_in_view) +
      statCard('NMEA bad CRC', s.nmea_bad_crc + ' / ' + s.nmea);
    document.getElementById('systemStats').innerHTML =
      statCard('Heap free', Math.round(s.heap_free / 1024) + 'k') +
      statCard('Heap min', Math.round(s.heap_min / 1024) + 'k') +
      statCard('Battery', s.batt_mv > 0 ? (s.batt_mv / 1000).toFixed(2) + 'V' : 'unknown') +
      statCard('WiFi clients', s.wifi_clients);
  }).catch(function () {});
}

function refreshRuns() {
  fetch('/api/runs').then(r => r.json()).then(function (runs) {
    var el = document.getElementById('runList');
    if (!runs.length) { el.innerHTML = '<div class="empty">No runs on this card yet.</div>'; return; }
    el.innerHTML = runs.slice().reverse().map(function (n) {
      var name = 'run' + String(n).padStart(4, '0');
      return '<div class="run"><span class="name">' + name + '</span><span class="links">' +
        '<a href="/api/runs/' + n + '/detections.csv">detections.csv</a>' +
        '<a href="/api/runs/' + n + '/session.csv">session.csv</a>' +
        '</span></div>';
    }).join('');
  }).catch(function () {
    document.getElementById('runList').innerHTML = '<div class="empty">Could not load run list.</div>';
  });
}

function loadConfig() {
  fetch('/api/config').then(r => r.json()).then(function (c) {
    document.getElementById('freq_mhz').value = c.freq_mhz;
    document.getElementById('sf').value = c.sf;
    document.getElementById('bw_khz').value = c.bw_khz;
    document.getElementById('cr_denom').value = c.cr_denom;
    document.getElementById('sync_word').value = '0x' + c.sync_word.toString(16).toUpperCase().padStart(2, '0');
  }).catch(function () {});
}

document.getElementById('configForm').addEventListener('submit', function (e) {
  e.preventDefault();
  var msg = document.getElementById('configMsg');
  msg.className = ''; msg.textContent = 'Saving…';
  var body = new URLSearchParams(new FormData(e.target));
  fetch('/api/config', { method: 'POST', body: body })
    .then(r => r.json().then(function (j) { return { ok: r.ok, j: j }; }))
    .then(function (res) {
      if (res.ok && res.j.ok) {
        msg.className = 'ok';
        msg.textContent = 'Saved — reboot the device to apply.';
      } else {
        msg.className = 'err';
        msg.textContent = 'Rejected: ' + (res.j.error || 'invalid values');
      }
    }).catch(function () {
      msg.className = 'err';
      msg.textContent = 'Save failed — device unreachable.';
    });
});

document.querySelectorAll('.tab-btn').forEach(function (btn) {
  btn.addEventListener('click', function () {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById('tab-' + btn.dataset.tab).classList.add('active');
    if (btn.dataset.tab === 'downloads') refreshRuns();
    if (btn.dataset.tab === 'settings') loadConfig();
  });
});

refreshStatus();
setInterval(refreshStatus, 2000);
</script>
</body>
</html>
)WEBPAGE";
