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
    /* Matches the running panel's RGB565 palette, not the design document's
       light/dark reader theme. The AP is offline, so use local system mono. */
    --bg: #000000;
    --surface: #000000;
    --surface-2: #101010;
    --border: #bdbebd;
    --border-soft: #515151;
    --text: #ffffff;
    --text-dim: #bdbebd;
    --brand: #4aa273;
    --good: #00ff00;
    --warn: #ffff00;
    --bad: #ff0000;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, "Liberation Mono", monospace;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
  }
  header {
    padding: 10px max(16px, calc((100vw - 960px) / 2 + 16px)) 0;
    border-bottom: 1px solid var(--text-dim);
  }
  .header-top {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 16px;
  }
  header h1 {
    margin: 0 0 10px;
    font-size: 18px;
    font-weight: 600;
    letter-spacing: 0;
  }
  header h1 span { color: var(--brand); }
  .header-indicators { display: flex; gap: 8px; padding-bottom: 10px; }
  .header-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--text-dim);
  }
  .header-dot.good { background: var(--good); }
  .header-dot.warn { background: var(--warn); }
  .header-dot.bad { background: var(--bad); }
  nav {
    display: flex;
    gap: 6px;
  }
  nav button {
    background: var(--bg);
    border: 1px solid transparent;
    color: var(--text-dim);
    padding: 7px 10px;
    font: inherit;
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.04em;
    text-transform: uppercase;
    cursor: pointer;
  }
  nav button.active {
    color: var(--bg);
    background: var(--text);
    border-color: var(--text);
  }
  nav button:not(.active):hover, nav button:focus-visible { color: var(--text); border-color: var(--text-dim); }
  main {
    padding: 20px 16px 48px;
    max-width: 960px;
    margin: 0 auto;
  }
  .tab { display: none; }
  .tab.active { display: block; }
  .status-identity {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 14px;
    border-bottom: 1px solid var(--text-dim);
    padding-bottom: 9px;
    color: var(--text-dim);
    font-size: 12px;
  }
  .status-identity b { color: var(--text); font-weight: 600; }
  .watch-readout, .operation {
    border: 1px solid var(--border-soft);
    padding: 12px;
  }
  .watch-readout { margin-top: 10px; }
  .readout-head, .operation-head {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    gap: 12px;
  }
  .readout-label, .operation-title, .operation-state {
    font-size: 12px;
    font-weight: 600;
    letter-spacing: 0.04em;
  }
  .readout-label, .operation-title { color: var(--text-dim); }
  .operation-state.good { color: var(--good); }
  .operation-state.warn { color: var(--warn); }
  .operation-state.bad { color: var(--bad); }
  .operation-state.dim { color: var(--text-dim); }
  .watch-figures {
    display: flex;
    align-items: baseline;
    gap: 18px;
    margin: 10px 0 8px;
    font-variant-numeric: tabular-nums;
  }
  .watch-figures strong { font-size: 28px; }
  .watch-figures span { font-size: 17px; color: var(--text-dim); }
  .watch-facts, .operation-facts {
    color: var(--text-dim);
    font-size: 12px;
    line-height: 1.5;
    font-variant-numeric: tabular-nums;
  }
  .operation-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    gap: 8px;
  }
  .operation-primary {
    margin: 12px 0 8px;
    font-size: 22px;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .operation-primary.good { color: var(--good); }
  .operation-primary.warn { color: var(--warn); }
  .operation-primary.bad { color: var(--bad); }
  .operation-primary.dim { color: var(--text-dim); }
  .operation-secondary {
    color: var(--text-dim);
    font-size: 13px;
    font-variant-numeric: tabular-nums;
  }
  .health-strip {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
    border: 1px solid var(--border-soft);
    font-variant-numeric: tabular-nums;
  }
  .health-item { padding: 9px 10px; border-right: 1px solid var(--border-soft); }
  .health-item:last-child { border-right: 0; }
  .health-item .label { color: var(--text-dim); font-size: 11px; margin-bottom: 5px; }
  .health-item .value { font-size: 13px; font-weight: 600; overflow-wrap: anywhere; }
  .health-item .value.good { color: var(--good); }
  .health-item .value.warn { color: var(--warn); }
  .health-item .value.bad { color: var(--bad); }
  .section-title {
    border-top: 1px solid var(--text-dim);
    padding-top: 7px;
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--text-dim);
    margin: 20px 0 8px;
  }
  .section-title:first-child { margin-top: 0; }
  .badge {
    font-size: 10px;
    text-transform: none;
    letter-spacing: normal;
    color: var(--good);
    border: 1px solid var(--good);
    border-radius: 0;
    padding: 1px 4px;
    vertical-align: middle;
  }
  .badge:empty { display: none; }
  form {
    background: var(--surface);
    border: 1px solid var(--border-soft);
    border-radius: 0;
    padding: 12px;
    display: grid;
    gap: 12px;
  }
  label {
    display: block;
    font-size: 12px;
    color: var(--text-dim);
    margin-bottom: 4px;
  }
  input, select {
    width: 100%;
    background: var(--surface-2);
    border: 1px solid var(--text-dim);
    border-radius: 0;
    color: var(--text);
    padding: 8px 10px;
    font: inherit;
    font-size: 14px;
    font-variant-numeric: tabular-nums;
  }
  input:focus { outline: 1px solid var(--good); outline-offset: 1px; }
  button.primary {
    background: var(--text);
    color: var(--bg);
    border: 1px solid var(--text);
    border-radius: 0;
    padding: 8px 12px;
    font: inherit;
    font-size: 13px;
    font-weight: 600;
    text-transform: uppercase;
    cursor: pointer;
    justify-self: start;
  }
  button.primary:hover, button.primary:focus-visible { background: var(--good); border-color: var(--good); }
  .note {
    font-size: 12px;
    color: var(--text-dim);
    line-height: 1.45;
    margin-top: 7px;
  }
  .settings-block {
    border-top: 1px solid var(--border-soft);
    margin-top: 22px;
    padding-top: 16px;
  }
  .settings-block:first-child { border-top: 0; margin-top: 0; padding-top: 0; }
  .settings-block .section-title { margin: 0 0 5px; }
  .settings-grid {
    display: grid;
    gap: 10px;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    margin-top: 12px;
  }
  .settings-grid .full { grid-column: 1 / -1; }
  .form-actions {
    align-items: center;
    display: flex;
    flex-wrap: wrap;
    gap: 10px;
    margin-top: 12px;
  }
  .form-msg { font-size: 13px; min-height: 18px; }
  .form-msg.ok { color: var(--good); }
  .form-msg.err { color: var(--bad); }
  .toggle-list {
    border: 1px solid var(--border-soft);
    margin-top: 12px;
  }
  .toggle-row {
    align-items: center;
    cursor: pointer;
    display: flex;
    gap: 14px;
    justify-content: space-between;
    margin: 0;
    padding: 11px 10px;
  }
  .toggle-row + .toggle-row { border-top: 1px solid var(--border-soft); }
  .toggle-copy { display: grid; gap: 3px; }
  .toggle-copy strong { color: var(--text); font-size: 13px; font-weight: 600; }
  .toggle-copy span { color: var(--text-dim); font-size: 11px; line-height: 1.35; }
  input.toggle-check { accent-color: var(--good); cursor: pointer; height: 18px; min-width: 18px; padding: 0; width: 18px; }
  .runlist { display: grid; gap: 12px; }
  .run {
    background: var(--surface);
    border: 1px solid var(--border-soft);
    border-radius: 0;
    padding: 12px;
  }
  .run-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    margin-bottom: 10px;
  }
  .run .name { font-size: 16px; font-weight: 600; font-variant-numeric: tabular-nums; }
  .run .meta { color: var(--text-dim); font-size: 11px; text-transform: uppercase; }
  .run-files {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
    gap: 6px;
  }
  .run-file {
    border: 1px solid var(--border-soft);
    color: var(--text);
    display: flex;
    flex-direction: column;
    gap: 5px;
    min-height: 58px;
    padding: 8px;
    text-decoration: none;
  }
  .run-file .file-kind { color: var(--text-dim); font-size: 10px; font-weight: 600; letter-spacing: 0.04em; }
  .run-file .file-name {
    color: var(--good);
    font-size: 13px;
  }
  .run-file:hover, .run-file:focus-visible { border-color: var(--good); outline: none; }
  .run-file:hover .file-name, .run-file:focus-visible .file-name { color: var(--text); text-decoration: underline; }
  .empty { color: var(--text-dim); font-size: 14px; padding: 20px 0; }
  @media (max-width: 520px) {
    header { padding-inline: 10px; }
    main { padding-inline: 10px; }
    .operation-grid { grid-template-columns: 1fr; }
    .health-strip { grid-template-columns: repeat(2, 1fr); }
    .health-item:nth-child(2n) { border-right: 0; }
    .health-item:nth-child(n + 3) { border-top: 1px solid var(--border-soft); }
    .settings-grid { grid-template-columns: 1fr; }
    .settings-grid .full { grid-column: auto; }
    nav { gap: 3px; }
    nav button { padding: 7px 6px; font-size: 11px; }
  }
</style>
</head>
<body>
<header>
  <div class="header-top">
    <h1>LoRaTrace <span>RX</span></h1>
    <div class="header-indicators" aria-label="GPS, heap, and receive health">
      <span id="gpsDot" class="header-dot" title="GPS"></span>
      <span id="heapDot" class="header-dot" title="Heap"></span>
      <span id="rxDot" class="header-dot" title="Receive activity"></span>
    </div>
  </div>
  <nav>
    <button class="tab-btn active" data-tab="status">Status</button>
    <button class="tab-btn" data-tab="downloads">Downloads</button>
    <button class="tab-btn" data-tab="settings">Settings</button>
  </nav>
</header>
<main>

  <section id="tab-status" class="tab active">
    <div class="status-identity" id="runReadout"></div>
    <div class="watch-readout" id="watchReadout"></div>
    <div class="section-title">Acquisition</div>
    <div class="operation-grid">
      <article class="operation" id="probeReadout"></article>
      <article class="operation" id="sweepReadout"></article>
    </div>
    <div class="note">Sweep peaks are measured energy, not LoRa evidence. A Pass-B packet count only changes after a received packet is promoted.</div>
    <div class="section-title">Health</div>
    <div class="health-strip" id="healthReadout"></div>
  </section>

  <section id="tab-downloads" class="tab">
    <div class="section-title">Run archive</div>
    <div class="note">Each run contains the same durable evidence set.</div>
    <div class="runlist" id="runList"><div class="empty">Loading…</div></div>
  </section>

  <section id="tab-settings" class="tab">
    <div class="settings-block">
      <div class="section-title">Display</div>
      <div class="note">Saved as the device's next-boot display preset.</div>
      <form id="displayForm">
        <div class="settings-grid">
          <div><label for="brightness_pct">Brightness (%)</label><input id="brightness_pct" name="brightness_pct" type="number" min="5" max="100" step="5"></div>
          <div><label for="idle_timeout_index">Idle dim</label><select id="idle_timeout_index" name="idle_timeout_index"><option value="0">Off</option><option value="1">30 seconds</option><option value="2">60 seconds</option><option value="3">2 minutes</option><option value="4">5 minutes</option></select></div>
        </div>
        <div class="form-actions"><button class="primary" type="submit">Save display</button><div id="displayMsg" class="form-msg"></div></div>
      </form>
    </div>

    <div class="settings-block">
      <div class="section-title">Capture &amp; diagnostics</div>
      <form id="optionsForm">
        <div class="toggle-list">
          <label class="toggle-row" for="identity_capture"><span class="toggle-copy"><strong>Node identity capture</strong><span>Write decoded MeshTastic identities to nodes.csv.</span></span><input id="identity_capture" class="toggle-check" type="checkbox"></label>
          <label class="toggle-row" for="verbose_debug"><span class="toggle-copy"><strong>Verbose serial debug</strong><span>Print per-packet summaries on the USB serial console.</span></span><input id="verbose_debug" class="toggle-check" type="checkbox"></label>
        </div>
        <div class="form-actions"><button class="primary" type="submit">Save options</button><div id="optionsMsg" class="form-msg"></div></div>
      </form>
      <div class="note">Radio acquisition, profile switching, SD recovery, Wi-Fi, and serial-access controls remain on-device.</div>
    </div>

    <div class="settings-block">
      <div class="section-title">Meshtastic preset <span id="mt_activeBadge" class="badge"></span></div>
      <form id="configForm-meshtastic" class="preset-form" data-profile="meshtastic" data-prefix="mt">
        <div class="settings-grid">
          <div><label for="mt_freq_mhz">Frequency (MHz)</label><input id="mt_freq_mhz" name="freq_mhz" type="number" step="0.001" min="868" max="928"></div>
          <div><label for="mt_sf">Spreading factor (SF5–SF12)</label><input id="mt_sf" name="sf" type="number" min="5" max="12"></div>
          <div><label for="mt_bw_khz">Bandwidth (kHz)</label><input id="mt_bw_khz" name="bw_khz" type="number" step="0.1" min="0.1"></div>
          <div><label for="mt_cr_denom">Coding rate (4/5–4/8)</label><input id="mt_cr_denom" name="cr_denom" type="number" min="5" max="8"></div>
          <div class="full"><label for="mt_sync_word">Sync word (hex, e.g. 0x2B)</label><input id="mt_sync_word" name="sync_word" type="text"></div>
        </div>
        <div class="form-actions"><button class="primary" type="submit">Save preset</button><div id="mt_configMsg" class="form-msg"></div></div>
      </form>
    </div>

    <div class="settings-block">
      <div class="section-title">MeshCore preset <span id="mc_activeBadge" class="badge"></span></div>
      <form id="configForm-meshcore" class="preset-form" data-profile="meshcore" data-prefix="mc">
        <div class="settings-grid">
          <div><label for="mc_freq_mhz">Frequency (MHz)</label><input id="mc_freq_mhz" name="freq_mhz" type="number" step="0.001" min="868" max="928"></div>
          <div><label for="mc_sf">Spreading factor (SF5–SF12)</label><input id="mc_sf" name="sf" type="number" min="5" max="12"></div>
          <div><label for="mc_bw_khz">Bandwidth (kHz)</label><input id="mc_bw_khz" name="bw_khz" type="number" step="0.1" min="0.1"></div>
          <div><label for="mc_cr_denom">Coding rate (4/5–4/8)</label><input id="mc_cr_denom" name="cr_denom" type="number" min="5" max="8"></div>
          <div class="full"><label for="mc_sync_word">Sync word (hex, e.g. 0x12)</label><input id="mc_sync_word" name="sync_word" type="text"></div>
        </div>
        <div class="form-actions"><button class="primary" type="submit">Save preset</button><div id="mc_configMsg" class="form-msg"></div></div>
      </form>
      <div class="note">Each channel preset is independent. Saves to /loratrace/config.txt and applies on the next boot; the running radio is not touched.</div>
    </div>
  </section>

</main>
<script>
function healthItem(label, value, cls) {
  return '<div class="health-item"><div class="label">' + label + '</div><div class="value' +
    (cls ? ' ' + cls : '') + '">' + value + '</div></div>';
}

function acquisitionClass(state, repeating) {
  if (state === 'FAILED') return 'bad';
  if (state === 'RUNNING' || state === 'CANCELLED' || repeating) return 'warn';
  if (state === 'COMPLETE') return 'good';
  return 'dim';
}

var lastRxCount = null;
function setHeaderDot(id, state) {
  document.getElementById(id).className = 'header-dot' + (state ? ' ' + state : '');
}

function refreshStatus() {
  fetch('/api/status').then(r => r.json()).then(function (s) {
    var totalDrops = s.queue_drop + s.row_drop + s.bus_miss;
    var heapUsed = Math.max(0, 512 - (s.heap_free / 1024)) / 512;
    setHeaderDot('gpsDot', s.has_fix ? 'good' : (s.sats_in_view > 0 ? 'warn' : 'bad'));
    setHeaderDot('heapDot', heapUsed >= 0.9 ? 'bad' : (heapUsed >= 0.8 ? 'warn' : 'good'));
    setHeaderDot('rxDot', lastRxCount !== null && s.rx !== lastRxCount ? 'good' : '');
    lastRxCount = s.rx;
    var probe = s.probe;
    var sweep = s.sweep;
    var probeCls = acquisitionClass(probe.state, false);
    var sweepCls = acquisitionClass(sweep.state, sweep.repeat_active);
    var sweepLabel = sweep.repeat_active ? 'REPEATING #' + sweep.repeat_count : sweep.state;
    var strongest = sweep.strongest_valid ?
      sweep.strongest_freq_mhz.toFixed(3) + ' MHz @ ' + sweep.strongest_rssi_dbm.toFixed(1) + ' dBm' : 'none';
    var probeEmpty = probe.state === 'IDLE' && probe.count === 0;
    var sweepEmpty = sweep.state === 'IDLE' && sweep.bin_count === 0;
    var gpsLabel = s.has_fix ? 'FIX · ' + s.sats + ' SATS' :
      (s.sats_in_view > 0 ? 'ACQUIRING · ' + s.sats_in_view + ' VIEW' : 'NO SKY');
    var gpsCls = s.has_fix ? 'good' : (s.sats_in_view > 0 ? 'warn' : 'bad');
    var heapCls = heapUsed >= 0.9 ? 'bad' : (heapUsed >= 0.8 ? 'warn' : 'good');
    document.getElementById('runReadout').innerHTML =
      '<span>RUN <b>r' + String(s.run).padStart(4, '0') + '</b></span>' +
      '<span>FW <b>v' + s.firmware_version + '</b></span>' +
      '<span>PROFILE <b>' + s.profile + '</b></span>' +
      '<span>HOME <b>' + s.home_freq_mhz.toFixed(3) + ' MHz</b></span>';
    document.getElementById('watchReadout').innerHTML =
      '<div class="readout-head"><span class="readout-label">WATCH</span><span class="operation-state ' +
      (s.trace_paused ? 'warn">STANDBY' : 'good">ACTIVE') + '</span></div>' +
      '<div class="watch-figures"><strong>rx ' + s.rx + '</strong><span>log ' + s.rows + '</span></div>' +
      '<div class="watch-facts">crc ' + s.crc_err + ' · drop ' + totalDrops +
      ' · flush ' + s.flushes + ' / max ' + s.max_flush_ms + 'ms</div>';
    document.getElementById('probeReadout').innerHTML =
      '<div class="operation-head"><span class="operation-title">PROBE</span><span class="operation-state ' +
      probeCls + '">' + probe.state + '</span></div>' +
      '<div class="operation-primary ' + probeCls + '">' +
      (probeEmpty ? 'NO PROBE YET' : probe.cad_detected + ' CAD HIT' + (probe.cad_detected === 1 ? '' : 'S')) +
      '</div><div class="operation-secondary">targets ' + probe.index + ' / ' + probe.count + '</div>' +
      '<div class="operation-facts">free ' + probe.cad_free + ' · timeout ' + probe.cad_timeout +
      ' · error ' + probe.errors + '</div>';
    document.getElementById('sweepReadout').innerHTML =
      '<div class="operation-head"><span class="operation-title">SWEEP</span><span class="operation-state ' +
      sweepCls + '">' + sweepLabel + '</span></div>' +
      '<div class="operation-primary ' + sweepCls + '">' +
      (sweepEmpty ? 'NO SWEEP YET' : sweep.peaks + ' ENERGY PEAK' + (sweep.peaks === 1 ? '' : 'S')) +
      '</div><div class="operation-secondary">bins ' + sweep.bin_index + ' / ' + sweep.bin_count +
      ' · strongest ' + strongest + '</div>' +
      '<div class="operation-facts">Pass-B CAD ' + sweep.pass_b_attempts +
      ' · packets ' + sweep.pass_b_detections + '</div>';
    document.getElementById('healthReadout').innerHTML =
      healthItem('SD', s.sd_ready ? 'OK' : 'DOWN', s.sd_ready ? 'good' : 'bad') +
      healthItem('GPS', gpsLabel, gpsCls) +
      healthItem('BATTERY', s.batt_mv > 0 ? (s.batt_mv / 1000).toFixed(2) + 'V' : 'UNKNOWN') +
      healthItem('HEAP', Math.round(s.heap_free / 1024) + 'k free', heapCls) +
      healthItem('WI-FI', s.wifi_clients + (s.wifi_clients === 1 ? ' client' : ' clients'));
  }).catch(function () {});
}

function runFileLink(n, kind, leaf) {
  return '<a class="run-file" href="/api/runs/' + n + '/' + leaf + '">' +
    '<span class="file-kind">' + kind + '</span><span class="file-name">' + leaf + '</span></a>';
}

function refreshRuns() {
  fetch('/api/runs').then(r => r.json()).then(function (runs) {
    var el = document.getElementById('runList');
    if (!runs.length) { el.innerHTML = '<div class="empty">No runs on this card yet.</div>'; return; }
    el.innerHTML = runs.slice().reverse().map(function (n) {
      var name = 'r' + String(n).padStart(4, '0');
      return '<article class="run"><div class="run-head"><span class="name">RUN ' + name +
        '</span><span class="meta">5 CSV files</span></div><div class="run-files">' +
        runFileLink(n, 'PACKETS', 'detections.csv') +
        runFileLink(n, 'HEALTH', 'session.csv') +
        runFileLink(n, 'PROBE', 'probe.csv') +
        runFileLink(n, 'ENERGY SWEEP', 'energy.csv') +
        runFileLink(n, 'NODES', 'nodes.csv') +
        '</div></article>';
    }).join('');
  }).catch(function () {
    document.getElementById('runList').innerHTML = '<div class="empty">Could not load run list.</div>';
  });
}

function fillPreset(prefix, c) {
  document.getElementById(prefix + '_freq_mhz').value = c.freq_mhz;
  document.getElementById(prefix + '_sf').value = c.sf;
  document.getElementById(prefix + '_bw_khz').value = c.bw_khz;
  document.getElementById(prefix + '_cr_denom').value = c.cr_denom;
  document.getElementById(prefix + '_sync_word').value = '0x' + c.sync_word.toString(16).toUpperCase().padStart(2, '0');
}

// Both presets come back from one GET — the device resolves each profile's
// override-or-default itself (channel_plans.h's resolvedChannelForProfile,
// same function a live profile switch uses), so the two panels always
// reflect what the radio would actually do, not just what's on the card.
function loadConfig() {
  fetch('/api/config').then(r => r.json()).then(function (c) {
    fillPreset('mt', c.meshtastic);
    fillPreset('mc', c.meshcore);
    document.getElementById('mt_activeBadge').textContent = c.active_profile === 'meshtastic' ? 'active' : '';
    document.getElementById('mc_activeBadge').textContent = c.active_profile === 'meshcore' ? 'active' : '';
  }).catch(function () {});
}

function loadDisplay() {
  fetch('/api/display').then(r => r.json()).then(function (d) {
    if (d.ok === false) return;
    document.getElementById('brightness_pct').value = d.brightness_pct;
    document.getElementById('idle_timeout_index').value = d.idle_timeout_index;
  }).catch(function () {});
}

function loadOptions() {
  fetch('/api/options').then(r => r.json()).then(function (o) {
    document.getElementById('identity_capture').checked = !!o.identity_capture;
    document.getElementById('verbose_debug').checked = !!o.verbose_debug;
  }).catch(function () {});
}

function loadSettings() {
  loadConfig();
  loadDisplay();
  loadOptions();
}

// One handler bound to both preset forms — the only per-form difference is
// which `profile` field rides along in the POST body, and data-profile on
// the <form> already carries that.
document.querySelectorAll('#configForm-meshtastic, #configForm-meshcore').forEach(function (form) {
  form.addEventListener('submit', function (e) {
    e.preventDefault();
    var prefix = form.dataset.prefix;
    var msg = document.getElementById(prefix + '_configMsg');
    msg.className = 'form-msg'; msg.textContent = 'Saving…';
    var body = new URLSearchParams(new FormData(e.target));
    body.set('profile', form.dataset.profile);
    fetch('/api/config', { method: 'POST', body: body })
      .then(r => r.json().then(function (j) { return { ok: r.ok, j: j }; }))
      .then(function (res) {
        if (res.ok && res.j.ok) {
          msg.className = 'form-msg ok';
          msg.textContent = 'Saved — reboot the device to apply.';
        } else {
          msg.className = 'form-msg err';
          msg.textContent = 'Rejected: ' + (res.j.error || 'invalid values');
        }
      }).catch(function () {
        msg.className = 'form-msg err';
        msg.textContent = 'Save failed — device unreachable.';
      });
  });
});

document.getElementById('displayForm').addEventListener('submit', function (e) {
  e.preventDefault();
  var msg = document.getElementById('displayMsg');
  msg.className = 'form-msg'; msg.textContent = 'Saving…';
  fetch('/api/display', { method: 'POST', body: new URLSearchParams(new FormData(e.target)) })
    .then(r => r.json().then(function (j) { return { ok: r.ok, j: j }; }))
    .then(function (res) {
      if (res.ok && res.j.ok) {
        msg.className = 'form-msg ok';
        msg.textContent = 'Saved — reboot the device to apply.';
      } else {
        msg.className = 'form-msg err';
        msg.textContent = 'Rejected: ' + (res.j.error || 'invalid values');
      }
    }).catch(function () {
      msg.className = 'form-msg err';
      msg.textContent = 'Save failed — device unreachable.';
    });
});

document.getElementById('optionsForm').addEventListener('submit', function (e) {
  e.preventDefault();
  var msg = document.getElementById('optionsMsg');
  msg.className = 'form-msg'; msg.textContent = 'Saving…';
  var body = new URLSearchParams({
    identity_capture: document.getElementById('identity_capture').checked ? '1' : '0',
    verbose_debug: document.getElementById('verbose_debug').checked ? '1' : '0'
  });
  fetch('/api/options', { method: 'POST', body: body })
    .then(r => r.json().then(function (j) { return { ok: r.ok, j: j }; }))
    .then(function (res) {
      if (res.ok && res.j.ok) {
        msg.className = 'form-msg ok';
        msg.textContent = 'Saved — active now.';
      } else {
        msg.className = 'form-msg err';
        msg.textContent = 'Rejected: ' + (res.j.error || 'invalid values');
      }
    }).catch(function () {
      msg.className = 'form-msg err';
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
    if (btn.dataset.tab === 'settings') loadSettings();
  });
});

refreshStatus();
setInterval(refreshStatus, 2000);
</script>
</body>
</html>
)WEBPAGE";
