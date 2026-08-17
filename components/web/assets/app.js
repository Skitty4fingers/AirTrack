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
      t('ehead', j.state === 'empty' ? 'No recent reports' : j.state === 'config_required' ? 'Set the tracking location to start' : 'Waiting for aircraft data');
      t('esub', 'within ' + j.radius_nm + ' NM · feed ' + j.state.replace('_', ' ') + (j.error !== 'none' ? ' · ' + j.error : ''));
    }
    var tb = $('rows');
    if (tb) {
      while (tb.firstChild) tb.removeChild(tb.firstChild);
      l.forEach(function (a) {
        var tr = document.createElement('tr');
        [name(a), a.type || '', dist(a.distance_nm), p3(a.bearing_deg) + '°', alt(a), a.speed_valid ? a.ground_speed_kt.toFixed(0) + ' kt' : '--', a.squawk || ''].forEach(function (v) {
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
  }
  function get(u, cb) { fetch(u, { cache: 'no-store' }).then(function (r) { return r.json(); }).then(cb).catch(function () {}); }
  function tick() { get('/api/v1/aircraft', air); get('/api/v1/status', st); }

  /* Form helpers: sliders <-> labels, fetch-based save with a toast. */
  var br = $('br'), brv = $('brv'); if (br && brv) br.oninput = function () { brv.textContent = br.value + '%'; };
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
