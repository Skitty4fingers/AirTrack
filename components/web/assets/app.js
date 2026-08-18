(function () {
  'use strict';
  function $(id) { return document.getElementById(id); }
  function t(id, v) { var e = $(id); if (e && e.textContent !== String(v)) e.textContent = v; }
  function p3(v) { v = Math.round(v) % 360; return (v < 100 ? (v < 10 ? '00' : '0') : '') + v; }
  function card(v) { return ['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'][Math.round(((v % 360) + 360) % 360 / 22.5) % 16]; }
  function up(s) { var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60); return (d ? d + 'd ' : '') + h + 'h ' + m + 'm'; }
  var unit = 'NM', f = 1;
  function dist(nm) { return (nm * f).toFixed(1) + ' ' + unit; }
  function alt(a) { return a.ground ? 'on the ground' : a.altitude_valid ? a.altitude_ft.toLocaleString() + ' ft' : '--'; }
  function name(a) { return a.callsign || a.registration || a.hex; }
  function vs(a) { if (!a.vertical_rate_valid || a.ground) return ''; var r = a.vertical_rate_fpm; return r === 0 ? ' · level' : ' · ' + (r > 0 ? '↑ ' : '↓ ') + Math.abs(r).toLocaleString() + ' fpm'; }
  function rot(id, deg) { var e = $(id); if (e) e.setAttribute('transform', 'rotate(' + deg.toFixed(1) + ' 60 60)'); }

  function air(j) {
    unit = j.unit || 'NM'; f = unit === 'km' ? 1.852 : unit === 'mi' ? 1.150779 : 1;
    var api = $('api');
    if (api) {
      var m = { live: ['API OK', 'ok'], empty: ['API OK', 'ok'], stale: ['API STALE', 'warn'], offline: ['API OFFLINE', 'bad'],
                searching: ['API …', ''], time_sync: ['TIME SYNC', 'warn'], config_required: ['SET LOCATION', 'warn'] }[j.state] || [j.state, ''];
      api.textContent = m[0]; api.className = m[1];
    }
    t('upd', j.last_success_age_s === null ? 'never' : j.last_success_age_s + 's ago');
    var l = j.aircraft || [], tg = $('target'), em = $('empty');
    if (l.length) {
      var a = l[0];
      t('id', name(a));
      t('meta', [a.type, a.registration !== name(a) ? a.registration : '', a.emergency ? 'EMERGENCY ' + (a.squawk || '') : ''].filter(Boolean).join(' · '));
      t('brg', dist(a.distance_nm) + ' · ' + card(a.bearing_deg) + ' · ' + p3(a.bearing_deg) + '°');
      t('alt', alt(a) + vs(a));
      t('spd', (a.speed_valid ? a.ground_speed_kt.toFixed(0) + ' kt' : '--') + (a.track_valid ? ' · trk ' + p3(a.track_deg) + '°' : ''));
      rot('arrow', a.bearing_deg); rot('plane', a.track_valid ? a.track_deg : a.bearing_deg);
      var arc = $('arc'); if (arc) { var s0 = (a.bearing_deg - 14) * Math.PI / 180, s1 = (a.bearing_deg + 14) * Math.PI / 180;
        arc.setAttribute('d', 'M' + (60 + 54 * Math.sin(s0)).toFixed(1) + ',' + (60 - 54 * Math.cos(s0)).toFixed(1) + ' A54,54 0 0 1 ' + (60 + 54 * Math.sin(s1)).toFixed(1) + ',' + (60 - 54 * Math.cos(s1)).toFixed(1)); arc.removeAttribute('hidden'); }
      if (tg) tg.hidden = false; if (em) em.hidden = true;
    } else {
      if (tg) tg.hidden = true; if (em) em.hidden = false;
      t('ehead', j.state === 'empty' ? (j.focus ? 'Waiting for ' + j.focus : 'No recent reports') : j.state === 'config_required' ? 'Set the tracking location to start' : 'Waiting for aircraft data');
      t('esub', 'within ' + j.radius_nm + ' NM · feed ' + j.state.replace('_', ' ') + (j.error !== 'none' ? ' · ' + j.error : ''));
    }
    var tb = $('rows');
    if (tb) {
      while (tb.firstChild) tb.removeChild(tb.firstChild);
      l.forEach(function (a) {
        var tr = document.createElement('tr');
        [name(a), a.type || '', a.route_from ? a.route_from + '→' + a.route_to : '', dist(a.distance_nm), p3(a.bearing_deg) + '°', alt(a), a.speed_valid ? a.ground_speed_kt.toFixed(0) + ' kt' : '--', a.squawk || ''].forEach(function (v) {
          var td = document.createElement('td'); td.textContent = v; tr.appendChild(td); });
        if (a.emergency) tr.className = 'emergency';
        tb.appendChild(tr);
      });
    }
    t('counts', j.accepted + ' shown of ' + j.reported + ' reports within ' + j.radius_nm + ' NM');
  }
  function st(j) {
    t('ssid', j.ssid); t('rssi', j.rssi_dbm === null ? 'unavailable' : j.rssi_dbm + ' dBm');
    t('uptime', up(j.uptime_s));
    t('heap', (j.free_heap_bytes / 1024).toFixed(0) + ' KiB free · min ' + (j.minimum_free_heap_bytes / 1024).toFixed(0) + ' KiB');
    t('polls', j.polls_ok + ' ok · ' + j.polls_failed + ' failed · ' + j.tls_connections + ' TLS sessions');
    t('sd', j.sd_mounted ? (j.sd_logging ? 'Logging · ' + j.sd_records + ' records' : 'Card mounted · logging off') : 'No card');
    t('sdsub', j.sd_mounted ? (j.sd_logging ? 'Enabled · ' + j.sd_records + ' records written' : 'Disabled · card ready') : 'No SD card detected');
    t('time', j.time_synchronized ? 'synchronized' : 'not yet synchronized');
    if (j.local_minutes !== undefined) { t('ltime', j.local_minutes < 0 ? '--:--' : ('0' + Math.floor(j.local_minutes / 60)).slice(-2) + ':' + ('0' + j.local_minutes % 60).slice(-2) + (j.night ? ' · night mode' : '')); t('nightsub', j.night ? 'Active now · panel dimmed' : 'Inactive'); }
    if (j.sd_log_bytes !== undefined) t('logusage', 'Using ' + (j.sd_log_bytes / 1048576).toFixed(1) + ' MiB in ' + j.sd_log_files + ' file' + (j.sd_log_files === 1 ? '' : 's') + ' of the cap · ' + j.sd_files_pruned + ' pruned');
  }

  /* Location helpers: browser geolocation (HTTPS only in most browsers) and a
     paste box that accepts "lat, lon" or a maps link containing @lat,lon. */
  function setLatLon(lat, lon) {
    var la = document.querySelector('[name=latitude]'), lo = document.querySelector('[name=longitude]');
    if (la) la.value = (+lat).toFixed(6); if (lo) lo.value = (+lon).toFixed(6);
    var h = $('geohint'); if (h) { h.textContent = 'Filled ' + (+lat).toFixed(5) + ', ' + (+lon).toFixed(5) + ' — press Save changes to apply.'; }
  }
  /* HTTPS helper page (docs/locate.html on GitHub Pages): browsers only share
     location on secure pages, so it reads the position there and navigates
     back to this dashboard with ?lat=&lon= filled in. */
  var HELPER = 'https://skitty4fingers.github.io/AirTrack/locate.html';
  function helperLink() {
    var a = document.createElement('a'); a.href = HELPER + '?back=' + encodeURIComponent(location.origin + '/');
    a.textContent = 'open the HTTPS locate helper'; a.rel = 'noopener'; return a;
  }
  (function fromHelper() {
    var q = new URLSearchParams(location.search), lat = q.get('lat'), lon = q.get('lon');
    if (lat === null || lon === null) return;
    if (isFinite(+lat) && isFinite(+lon) && Math.abs(+lat) <= 90 && Math.abs(+lon) <= 180) {
      setLatLon(lat, lon);
      var h = $('geohint'); if (h) h.textContent = 'Location received from the helper: ' + (+lat).toFixed(5) + ', ' + (+lon).toFixed(5) + ' — press Save changes to apply.';
      var loc = $('location'); if (loc) loc.scrollIntoView();
    }
    history.replaceState(null, '', location.pathname + location.hash);
  })();
  var geo = $('geo');
  if (geo) geo.onclick = function () {
    var h = $('geohint');
    if (!window.isSecureContext) {
      if (h) { h.textContent = 'This page is plain HTTP, so the browser will not share location here — '; h.appendChild(helperLink()); h.appendChild(document.createTextNode(' (it asks once and brings you straight back), or paste coordinates.')); }
      return;
    }
    if (!navigator.geolocation) { if (h) h.textContent = 'This browser has no geolocation API. Paste coordinates instead.'; return; }
    if (h) h.textContent = 'Asking the browser for your location…';
    navigator.geolocation.getCurrentPosition(function (p) { setLatLon(p.coords.latitude, p.coords.longitude); },
      function (e) { if (h) { h.textContent = (e.code === 1 ? 'Location permission was denied. ' : 'Location unavailable. ') + 'You can '; h.appendChild(helperLink()); h.appendChild(document.createTextNode(' or paste coordinates from a maps app.')); } },
      { enableHighAccuracy: true, timeout: 12000, maximumAge: 60000 });
  };
  var paste = $('paste');
  if (paste) paste.oninput = function () {
    var v = paste.value, m = v.match(/@(-?\d+\.\d+),(-?\d+\.\d+)/) || v.match(/(-?\d{1,3}\.\d+)[,\s]+(-?\d{1,3}\.\d+)/) || v.match(/[?&]q=(-?\d+\.\d+),(-?\d+\.\d+)/);
    if (m && Math.abs(+m[1]) <= 90 && Math.abs(+m[2]) <= 180) { setLatLon(m[1], m[2]); paste.value = ''; }
  };

  /* Log viewer. */
  var current = null;
  function human(b) { return b >= 1048576 ? (b / 1048576).toFixed(1) + ' MiB' : b >= 1024 ? (b / 1024).toFixed(0) + ' KiB' : b + ' B'; }
  function loadLogs() {
    var list = $('loglist'); if (!list) return;
    fetch('/api/v1/logs', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (j) {
      while (list.firstChild) list.removeChild(list.firstChild);
      if (!j.mounted) { list.textContent = 'No SD card.'; return; }
      if (!j.files.length) { list.textContent = 'No log files yet.'; return; }
      j.files.forEach(function (f) {
        var b = document.createElement('button'); b.type = 'button'; b.textContent = f.name;
        var sm = document.createElement('small'); sm.textContent = human(f.bytes); b.appendChild(sm);
        if (f.name === current) b.className = 'on';
        b.onclick = function () { showLog(f.name); };
        list.appendChild(b);
      });
    }).catch(function () { list.textContent = 'Log list unavailable.'; });
  }
  function showLog(name) {
    current = name; loadLogs();
    var view = $('logview'), rows = $('logrows'), dl = $('logdl');
    if (!view || !rows) return;
    view.hidden = false; t('logname', name + ' (last 48 KiB)');
    if (dl) dl.href = '/api/v1/logs/' + name + '?download=1';
    fetch('/api/v1/logs/' + name + '?tail=49152', { cache: 'no-store' }).then(function (r) { return r.text(); }).then(function (txt) {
      while (rows.firstChild) rows.removeChild(rows.firstChild);
      var lines = txt.split('\n').filter(Boolean).reverse().slice(0, 300);
      lines.forEach(function (line) {
        var r; try { r = JSON.parse(line); } catch (e) { return; }
        var tr = document.createElement('tr');
        [r.ts ? r.ts.replace('T', ' ').replace('Z', '') : 'unsynced +' + Math.floor(r.mono_ms / 1000) + 's', r.event || r.state || '', r.flight || r.hex || '', r.reg || '', r.type || '', r.route || '',
         (r.dst_nm !== undefined ? r.dst_nm.toFixed(1) + ' NM' : ''), (r.ground ? 'ground' : r.alt_ft !== undefined ? r.alt_ft + ' ft' : ''), (r.gs_kt !== undefined ? r.gs_kt.toFixed(0) + ' kt' : '')]
          .forEach(function (v) { var td = document.createElement('td'); td.textContent = v; tr.appendChild(td); });
        if (r.emergency) tr.className = 'emergency';
        rows.appendChild(tr);
      });
      if (!rows.firstChild) { var tr = document.createElement('tr'), td = document.createElement('td'); td.colSpan = 9; td.textContent = 'Empty file.'; tr.appendChild(td); rows.appendChild(tr); }
    }).catch(function () { t('logname', name + ' — could not load'); });
  }
  /* Firmware updates: check → install → progress → reconnect. */
  function csrfForm(action) {
    var f = document.createElement('form'); f.action = action;
    var tok = document.querySelector('#cfg [name=csrf_token]');
    var i = document.createElement('input'); i.name = 'csrf_token'; i.value = tok ? tok.value : ''; f.appendChild(i);
    return f;
  }
  var otaTimer = null, otaVersion = '';
  function ago(sec) {
    if (sec == null || sec < 0) return '';
    if (sec < 60) return 'checked just now';
    if (sec < 3600) return 'checked ' + Math.floor(sec / 60) + ' min ago';
    return 'checked ' + Math.floor(sec / 3600) + ' h ago';
  }
  function otaRender(o) {
    var sub = $('otasub'), inst = $('otainstall'), bar = $('otabar'), fill = $('otafill');
    if (!sub) return;
    otaVersion = o.available || '';
    var text = { idle: 'Not checked yet', checking: 'Checking…',
      up_to_date: 'You are on the latest version',
      available: 'A newer version is available',
      downloading: 'Downloading ' + o.available + ' … ' + o.percent + '%' +
        (o.size ? ' (' + Math.round(o.downloaded / 1024) + ' / ' + Math.round(o.size / 1024) + ' KiB)' : ''),
      verifying: 'Verifying image…', ready: 'Installed — restarting…',
      failed: 'Update failed: ' + (o.error || '') }[o.state] || o.state;
    sub.textContent = text;
    var latest = $('otalatest'), checked = $('otachecked');
    if (latest) latest.textContent = o.available || '—';
    if (checked) {
      var parts = [];
      if (o.released) parts.push('released ' + o.released);
      var a = ago(o.checked_age_s); if (a) parts.push(a);
      checked.textContent = o.available ? parts.join(' · ') : (o.state === 'checking' ? 'checking…' : 'press Check for updates');
    }
    var notes = $('otanotes'), np = $('otanotesp'), nv = $('otanotesv');
    if (notes) {
      notes.hidden = !(o.available && o.notes);
      if (np) np.textContent = o.notes || '';
      if (nv) nv.textContent = (o.available || '') + (o.state === 'up_to_date' ? ' (installed)' : '');
    }
    if (inst) { inst.hidden = o.state !== 'available'; inst.textContent = 'Install ' + (o.available || ''); }
    var oc = $('otacheck'); if (oc) oc.disabled = !!(o.busy || o.state === 'checking' || o.state === 'downloading' || o.state === 'verifying' || o.state === 'ready');
    if (bar) { bar.hidden = !(o.state === 'downloading' || o.state === 'verifying' || o.state === 'ready'); }
    if (fill) fill.style.width = (o.state === 'ready' || o.state === 'verifying' ? 100 : o.percent) + '%';
    var still = o.state === 'checking' || o.state === 'downloading' || o.state === 'verifying' || o.busy;
    if (o.state === 'ready') { waitForReboot(o.available); return; }
    if (still && !otaTimer) otaTimer = setInterval(otaPoll, 1000);
    if (!still && otaTimer) { clearInterval(otaTimer); otaTimer = null; }
  }
  function otaPoll() { fetch('/api/v1/ota/status', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(otaRender).catch(function () {}); }
  function waitForReboot(version) {
    if (otaTimer) { clearInterval(otaTimer); otaTimer = null; }
    var sub = $('otasub'); if (sub) sub.textContent = 'Restarting into ' + version + ' — reconnecting…';
    var tries = 0;
    var t = setInterval(function () {
      tries++;
      fetch('/api/v1/status', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (j) {
        if (j.firmware === version) { clearInterval(t); location.reload(); }
      }).catch(function () {});
      if (tries > 120) { clearInterval(t); if (sub) sub.textContent = 'Device did not come back on this address; check the LCD.'; }
    }, 2000);
  }
  var oc = $('otacheck');
  if (oc) oc.onclick = function () {
    post(csrfForm('/api/v1/ota/check'), function (r) {
      if (!r.ok) { toast('Check failed: ' + (r.j.error || ''), 'bad'); return; }
      otaRender({ state: 'checking', busy: true, percent: 0, available: otaVersion });
    });
  };
  var oi = $('otainstall');
  if (oi) oi.onclick = function () {
    if (!otaVersion || !confirm('Install AirTrack ' + otaVersion + '? The display pauses tracking during the download and restarts when done.')) return;
    var f = csrfForm('/api/v1/ota/start');
    var v = document.createElement('input'); v.name = 'version'; v.value = otaVersion; f.appendChild(v);
    post(f, function (r) { if (!r.ok) { toast('Not started: ' + (r.j.error || ''), 'bad'); return; } otaRender({ state: 'downloading', busy: true, percent: 0, available: otaVersion }); });
  };
  otaPoll();

  var lr = $('logrefresh'); if (lr) lr.onclick = loadLogs;
  var lc = $('logclear');
  if (lc) lc.onclick = function () {
    if (!confirm('Delete every sighting log file on the SD card?')) return;
    var f = document.createElement('form'); f.action = '/api/v1/logs/clear';
    var tok = document.querySelector('#cfg [name=csrf_token]');
    var i = document.createElement('input'); i.name = 'csrf_token'; i.value = tok ? tok.value : ''; f.appendChild(i);
    post(f, function (r) { toast(r.ok ? 'Log cleared' : 'Not cleared: ' + (r.j.error || 'rejected'), r.ok ? 'ok' : 'bad'); current = null; var v = $('logview'); if (v) v.hidden = true; loadLogs(); tick(); });
  };
  loadLogs();
  function get(u, cb) { fetch(u, { cache: 'no-store' }).then(function (r) { return r.json(); }).then(cb).catch(function () {}); }
  function tick() { get('/api/v1/aircraft', air); get('/api/v1/status', st); }

  /* Form helpers: sliders <-> labels, fetch-based save with a toast. */
  var br = $('br'), brv = $('brv'); if (br && brv) br.oninput = function () { brv.textContent = br.value + '%'; };
  var nbr = $('nbr'), nbrv = $('nbrv'); if (nbr && nbrv) nbr.oninput = function () { nbrv.textContent = nbr.value + '%'; };

  /* Timezone presets: IANA name -> POSIX rule the device understands. The
     browser's zone is pre-selected when the device has none yet. */
  var TZ = [
    ['UTC', 'UTC0'], ['America/Anchorage', 'AKST9AKDT,M3.2.0,M11.1.0'], ['America/Los_Angeles', 'PST8PDT,M3.2.0,M11.1.0'],
    ['America/Vancouver', 'PST8PDT,M3.2.0,M11.1.0'], ['America/Denver', 'MST7MDT,M3.2.0,M11.1.0'], ['America/Phoenix', 'MST7'],
    ['America/Chicago', 'CST6CDT,M3.2.0,M11.1.0'], ['America/New_York', 'EST5EDT,M3.2.0,M11.1.0'], ['America/Toronto', 'EST5EDT,M3.2.0,M11.1.0'],
    ['America/Halifax', 'AST4ADT,M3.2.0,M11.1.0'], ['America/Sao_Paulo', '<-03>3'], ['Pacific/Honolulu', 'HST10'],
    ['Europe/London', 'GMT0BST,M3.5.0/1,M10.5.0'], ['Europe/Dublin', 'IST-1GMT0,M10.5.0,M3.5.0/1'], ['Europe/Lisbon', 'WET0WEST,M3.5.0/1,M10.5.0'],
    ['Europe/Paris', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Europe/Berlin', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Europe/Madrid', 'CET-1CEST,M3.5.0,M10.5.0/3'],
    ['Europe/Rome', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Europe/Amsterdam', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Europe/Stockholm', 'CET-1CEST,M3.5.0,M10.5.0/3'],
    ['Europe/Helsinki', 'EET-2EEST,M3.5.0/3,M10.5.0/4'], ['Europe/Athens', 'EET-2EEST,M3.5.0/3,M10.5.0/4'], ['Europe/Moscow', 'MSK-3'],
    ['Asia/Dubai', '<+04>-4'], ['Asia/Kolkata', 'IST-5:30'], ['Asia/Singapore', '<+08>-8'], ['Asia/Hong_Kong', 'HKT-8'],
    ['Asia/Shanghai', 'CST-8'], ['Asia/Tokyo', 'JST-9'], ['Asia/Seoul', 'KST-9'], ['Australia/Perth', 'AWST-8'],
    ['Australia/Brisbane', 'AEST-10'], ['Australia/Sydney', 'AEST-10AEDT,M10.1.0,M4.1.0/3'], ['Australia/Melbourne', 'AEST-10AEDT,M10.1.0,M4.1.0/3'],
    ['Australia/Adelaide', 'ACST-9:30ACDT,M10.1.0,M4.1.0/3'], ['Pacific/Auckland', 'NZST-12NZDT,M9.5.0,M4.1.0/3']];
  var tzsel = $('tzsel'), tz = $('tz');
  if (tzsel && tz) {
    TZ.forEach(function (z) { var o = document.createElement('option'); o.value = z[1]; o.textContent = z[0]; tzsel.appendChild(o); });
    var mine = ''; try { mine = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch (e) {}
    var match = TZ.filter(function (z) { return z[1] === tz.value; })[0];
    if (match) tzsel.value = match[1];
    else if (!tz.value && mine) { var m2 = TZ.filter(function (z) { return z[0] === mine; })[0]; if (m2) { tzsel.value = m2[1]; tz.value = m2[1]; var h = $('tzhint'); if (h) h.textContent = 'Detected ' + mine + ' from this browser — press Save changes to keep it.'; } }
    tzsel.onchange = function () { if (tzsel.value) tz.value = tzsel.value; };
    tz.oninput = function () { var m3 = TZ.filter(function (z) { return z[1] === tz.value; })[0]; tzsel.value = m3 ? m3[1] : ''; };
  }
  var rad = $('rad'), radn = $('radn');
  if (rad && radn) { rad.oninput = function () { radn.value = rad.value; }; radn.oninput = function () { var v = Math.min(250, Math.max(1, +radn.value || 1)); rad.value = v; }; }
  function toast(msg, cls) { var e = $('toast'); if (!e) return; e.textContent = msg; e.className = 'toast ' + (cls || ''); if (cls === 'ok') setTimeout(function () { if (e.textContent === msg) e.textContent = ''; }, 5000); }
  function post(form, done) {
    var body = new URLSearchParams(new FormData(form)).toString();
    return fetch(form.action, { method: 'POST', body: body, headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json' } })
      .then(function (r) { return r.json().then(function (j) { return { ok: r.ok && j.ok, j: j }; }); })
      .then(done).catch(function () { done({ ok: false, j: { error: 'device unreachable' } }); });
  }
  var cfg = $('cfg');
  if (cfg) cfg.onsubmit = function (e) {
    e.preventDefault(); toast('Saving…');
    post(cfg, function (r) { toast(r.ok ? 'Saved ✓ settings applied' : 'Not saved: ' + (r.j.error || 'rejected'), r.ok ? 'ok' : 'bad'); if (r.ok) tick(); });
  };
  var rb = $('rb');
  if (rb) rb.onsubmit = function (e) {
    e.preventDefault(); if (!confirm('Restart AirTrack now?')) return;
    post(rb, function (r) { toast(r.ok ? 'Restarting…' : 'Restart rejected', r.ok ? 'ok' : 'bad'); });
  };
  var fr = $('factory');
  if (fr) fr.onclick = function () {
    var word = prompt('This erases the SD sighting log, Wi-Fi, location and all options, and returns AirTrack to a brand-new device (new setup hotspot password, shown on the LCD).\n\nType RESET to continue.');
    if (word === null) return;
    var f = document.createElement('form'); f.action = '/api/v1/factory-reset';
    var tok = document.querySelector('#rb [name=csrf_token]');
    [['csrf_token', tok ? tok.value : ''], ['confirm', word.trim().toUpperCase()]].forEach(function (kv) { var i = document.createElement('input'); i.name = kv[0]; i.value = kv[1]; f.appendChild(i); });
    post(f, function (r) { toast(r.ok ? 'Factory reset — restarting into setup mode. Look at the LCD for the new hotspot.' : 'Not reset: ' + (r.j.error || 'rejected'), r.ok ? 'ok' : 'bad'); });
  };

  /* Sidebar highlight follows the section in view. */
  var links = Array.prototype.slice.call(document.querySelectorAll('.side nav a'));
  if ('IntersectionObserver' in window && links.length) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) { if (en.isIntersecting) links.forEach(function (a) { a.classList.toggle('active', a.getAttribute('href') === '#' + en.target.id); }); });
    }, { rootMargin: '-40% 0px -55% 0px' });
    links.forEach(function (a) { var s = document.querySelector(a.getAttribute('href')); if (s) io.observe(s); });
  }
  tick(); setInterval(tick, 2000);
})();
