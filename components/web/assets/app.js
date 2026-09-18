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
      t('meta', [a.desc || a.type, a.registration !== name(a) ? a.registration : '', a.emergency ? 'EMERGENCY ' + (a.squawk || '') : ''].filter(Boolean).join(' · '));
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
        [name(a), a.desc || a.type || '', a.route_from ? a.route_from + '→' + a.route_to : '', dist(a.distance_nm), p3(a.bearing_deg) + '°', alt(a), a.speed_valid ? a.ground_speed_kt.toFixed(0) + ' kt' : '--', a.squawk || ''].forEach(function (v, i) {
          var td = document.createElement('td');
          if (i === 0 && a.airline_iata) td.appendChild(logoImg(a.airline_iata, 36, 'tlogo'));
          td.appendChild(document.createTextNode(v)); tr.appendChild(td); });
        if (a.emergency) tr.className = 'emergency';
        tb.appendChild(tr);
      });
    }
    t('counts', j.accepted + ' shown of ' + j.reported + ' reports within ' + j.radius_nm + ' NM');
    /* Following one flight: the flight card replaces the nearest block. */
    focus = j.focus || '';
    var fl = $('flight'), ne = $('nearest');
    if (fl) fl.hidden = !focus; if (ne) ne.hidden = !!focus;
    t('dtitle', focus ? 'Tracking ' + focus : 'Nearest aircraft');
    lastAir = j;
    if (focus) renderFlight();
  }

  /* ---- Followed flight ---- */
  var focus = '', lastAir = null, lastFlight = null;
  /* Airline logos come straight from pics.avs.io (allowed by the page CSP);
     a code it does not know simply leaves the image out. */
  function logoImg(iata, px, cls) {
    var i = document.createElement('img'); i.className = cls; i.alt = ''; i.loading = 'lazy';
    i.referrerPolicy = 'no-referrer';
    i.src = 'https://pics.avs.io/al_square/' + px + '/' + px + '/' + encodeURIComponent(iata) + '.png';
    i.onerror = function () { i.remove(); };
    return i;
  }
  function hm(ts) { return new Date(ts * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }); }
  function dur(s) { s = Math.max(0, Math.round(s / 60)); return s >= 60 ? Math.floor(s / 60) + 'h ' + ('0' + s % 60).slice(-2) + 'm' : s + ' min'; }
  function since(s) { return s < 90 ? Math.round(s) + ' s ago' : s < 5400 ? Math.round(s / 60) + ' min ago' : (s / 3600).toFixed(1) + ' h ago'; }
  function grouped(n) { return Math.round(n).toLocaleString(); }
  function delayText(m) { return m === null || m === undefined ? '' : m > 0 ? ' (+' + m + ' min)' : m < 0 ? ' (' + m + ' min)' : ' (on time)'; }
  function parts() { return Array.prototype.filter.call(arguments, Boolean).join(' · '); }
  var PHASE = {
    unknown: ['Waiting', 'dim'], ground: ['At the airport', 'dim'], climb: ['Climbing', 'ok'], cruise: ['En route', 'ok'],
    descent: ['Descending', 'ok'], approach: ['On approach', 'ok'], landed: ['Landed', 'ok'], lost: ['No signal', 'warn'] };
  var NOTE = {
    no_key: 'Add a Flystack key under Flight data for scheduled times, delays, gates and airframe details.',
    idle: 'Fetching the schedule from Flystack…',
    not_found: 'Flystack has no schedule for this flight number.',
    unauthorized: 'Flystack refused the key for flight lookups (HTTP 401/403). Check that the key is active and allowed to use Flight Lookup in the Flystack dashboard.',
    quota: 'The Flystack quota or this device\'s daily limit is used up; schedules resume when it resets.',
    error: 'Flystack could not be reached; retrying in a few minutes.',
    unsupported: 'Schedules are only available for airline flight numbers (e.g. ASA555).' };

  /* Great-circle distance in NM between [lat, lon] pairs. */
  function gcd(p, q) {
    var r = Math.PI / 180, a = Math.pow(Math.sin((q[0] - p[0]) * r / 2), 2) +
      Math.cos(p[0] * r) * Math.cos(q[0] * r) * Math.pow(Math.sin((q[1] - p[1]) * r / 2), 2);
    return 3440.065 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
  }
  function mins(d) { return d > 0 ? d + ' min late' : d < 0 ? -d + ' min early' : 'on time'; }

  /*
   * Precedence: what ADS-B shows (adsb.fi, with the adsbdb route for its
   * callsign) wins for identity, airframe, route, phase, and the times it
   * observed; Flystack fills in what ADS-B cannot know (timetable, delays,
   * gates, belt) and stands in before the flight is seen.  Clock times come
   * from the device, in its own time zone, like the LCD.
   */
  function renderFlight() {
    var fl = lastFlight, j = lastAir; if (!fl || !j) return;
    var a = (j.aircraft || [])[0], s = fl.schedule, r = fl.route, tm = fl.times || {};
    if (a && [a.callsign, a.registration, a.hex].indexOf(focus) < 0 && [a.callsign, a.registration, a.hex].indexOf(fl.code) < 0) a = null;
    var zone = tm.zone ? ' ' + tm.zone : '';
    /* Header.  The logo comes from the device, which fetched it. */
    var lg = $('flogo');
    if (lg) {
      if (fl.logo_iata && lg.getAttribute('data-iata') !== fl.logo_iata) {
        lg.setAttribute('data-iata', fl.logo_iata); lg.hidden = false;
        lg.onerror = function () { lg.hidden = true; };
        lg.src = '/api/v1/flight/logo.png?a=' + encodeURIComponent(fl.logo_iata);
      } else if (!fl.logo_iata) { lg.hidden = true; lg.removeAttribute('data-iata'); }
    }
    t('fcs', (a && a.callsign) || r.callsign_icao || focus);
    var number = r.callsign_iata || (s && s.flight_iata) || '';
    t('fairline', parts(r.airline, number && number !== ((a && a.callsign) || r.callsign_icao || focus) ? number.replace(/^([A-Z0-9]{2})(\d)/, '$1 $2') : '', (a && a.registration) || (s && s.registration)));
    var ph = PHASE[fl.phase] || PHASE.unknown, label = ph[0], cls = ph[1];
    if (fl.phase === 'unknown' && s && s.status) { label = s.status.replace('-', ' '); cls = /cancel/.test(s.status) ? 'bad' : 'dim'; }
    if (fl.phase === 'ground' && a && a.speed_valid && a.ground_speed_kt > 5) label = 'Taxiing';
    if (a && a.emergency) { label = 'Emergency ' + (a.squawk || ''); cls = 'bad'; }
    var pill = $('fstatus'); if (pill) { pill.textContent = label; pill.className = 'pill ' + cls; }
    /* Route and progress. */
    /* Route order: as ADS-B has seen the aircraft fly it; before that,
       Flystack's leg for today; before that, adsbdb's usual order, which
       for out-and-back flight numbers may be backwards. */
    var conf = fl.route_confirmed || !!(a && a.route_confirmed);
    var from = conf ? (a && a.route_from) || r.from : (s && s.from) || r.from || (a && a.route_from) || '';
    var to = conf ? (a && a.route_to) || r.to : (s && s.to) || r.to || (a && a.route_to) || '';
    function city(code) { return code && code === r.from ? r.from_city : code && code === r.to ? r.to_city : ''; }
    t('ffrom', from || '—'); t('fto', to || '—'); t('ffromcity', city(from)); t('ftocity', city(to));
    var now = Date.now() / 1000, depEst = s && s.dep_time ? s.dep_time + 60 * (s.dep_delay_min || 0) : 0,
        arrEst = s && s.arr_time ? s.arr_time + 60 * (s.arr_delay_min || 0) : 0;
    var p = a && a.progress !== null && a.progress !== undefined ? a.progress : null;
    if (fl.phase === 'landed') p = 1;
    /* Not reporting yet means still at the origin; on the ground with the
       direction unconfirmed it could be either end, so no marker. */
    if (p === null && !fl.was_airborne && !a) p = 0;
    if (p === null && depEst && arrEst > depEst) p = Math.min(1, Math.max(0, (now - depEst) / (arrEst - depEst)));
    var pct = p === null ? 0 : Math.round(p * 1000) / 10;
    var fill = $('ffill'), dot = $('fdot');
    if (fill) fill.style.width = pct + '%'; if (dot) { dot.style.left = pct + '%'; dot.hidden = p === null; }
    var prog = '';
    if (fl.phase === 'landed') prog = 'Arrived';
    else if (a && conf && fl.origin && fl.destination && fl.was_airborne) {
      var here = [a.lat, a.lon];
      prog = parts(grouped(gcd(fl.origin, here) * f) + ' ' + unit + ' flown', grouped(gcd(here, fl.destination) * f) + ' ' + unit + ' to go');
    } else if (fl.origin && fl.destination) prog = grouped(gcd(fl.origin, fl.destination) * f) + ' ' + unit;
    t('fprog', prog);
    /* Departure: the takeoff ADS-B saw beats the timetable. */
    var dsub = [], asub = [], depLate = 0, arrLate = 0;
    if (tm.takeoff_local) {
      t('fdep', 'Departed ' + tm.takeoff_local + zone);
      if (s && s.dep_time) { depLate = Math.round((tm.takeoff - s.dep_time) / 60); dsub.push(mins(depLate) + ', scheduled ' + tm.dep_sched_local); }
    } else if (tm.dep_est_local) {
      t('fdep', (fl.was_airborne ? 'Departed ~' : '') + tm.dep_est_local + zone);
      if (s.dep_delay_min !== null) { depLate = s.dep_delay_min; dsub.push(s.dep_delay_min ? mins(s.dep_delay_min) + ', scheduled ' + tm.dep_sched_local : 'on time'); }
    } else t('fdep', fl.was_airborne ? 'Departed' : '—');
    if (s) { if (s.dep_terminal) dsub.push('Terminal ' + s.dep_terminal); if (s.dep_gate) dsub.push('Gate ' + s.dep_gate); }
    /* Arrival: touchdown, then the live estimate from ground speed, then Flystack. */
    if (tm.landing_local) {
      t('farr', 'Landed ' + tm.landing_local + zone);
      if (s && s.arr_time) { arrLate = Math.round((tm.landing - s.arr_time) / 60); asub.push(mins(arrLate) + ', scheduled ' + tm.arr_sched_local); }
    } else if (fl.phase === 'landed') {
      t('farr', 'Landed');
    } else if (tm.eta_local) {
      t('farr', 'ETA ' + tm.eta_local + zone); asub.push('in ' + dur(tm.eta - now) + ' at current speed');
      if (s && s.arr_time) { arrLate = Math.round((tm.eta - s.arr_time) / 60); asub.push('scheduled ' + tm.arr_sched_local); }
    } else if (tm.arr_est_local) {
      t('farr', tm.arr_est_local + zone);
      if (s.arr_delay_min) { arrLate = s.arr_delay_min; asub.push(mins(s.arr_delay_min) + ', scheduled ' + tm.arr_sched_local); }
    } else t('farr', '—');
    if (s) { if (s.arr_terminal) asub.push('Terminal ' + s.arr_terminal); if (s.arr_gate) asub.push('Gate ' + s.arr_gate); if (s.arr_baggage) asub.push('Belt ' + s.arr_baggage); }
    var dep = $('fdep'); if (dep) dep.className = depLate > 14 ? 'late' : '';
    var arr = $('farr'); if (arr) arr.className = arrLate > 14 ? 'late' : arrLate < -4 ? 'early' : '';
    t('fdepsub', dsub.join(' · ')); t('farrsub', asub.join(' · '));
    /* Live facts. */
    if (a) {
      t('falt', alt(a) + vs(a));
      t('fspd', (a.speed_valid ? a.ground_speed_kt.toFixed(0) + ' kt' : '--') + (a.track_valid ? ' · trk ' + p3(a.track_deg) + '°' : ''));
      t('fpos', dist(a.distance_nm) + ' from here · ' + card(a.bearing_deg) + ' ' + p3(a.bearing_deg) + '°');
      t('fage', a.age_s > 60 ? 'Last position ' + since(a.age_s) : 'Live · position ' + Math.round(a.age_s) + ' s old');
    } else {
      t('falt', '--'); t('fspd', '--'); t('fpos', 'Searching adsb.fi worldwide');
      t('fage', fl.phase === 'landed' ? 'Landed · transponder off' : fl.was_airborne ? 'Signal lost' : 'Not reporting yet');
    }
    var year = (a && a.year) || (s && s.built);
    /* The airframe's name rather than its ICAO designator, which is only a fallback. */
    t('fcraft', parts((a && a.desc) || (s && s.model) || (a && a.type) || (s && s.aircraft), year ? 'built ' + year : '', a ? 'ICAO ' + a.hex : ''));
    t('fnote', NOTE[fl.schedule_state] || (s && s.fetched ? 'Timetable, delays and gates from Flystack, updated ' + since(now - s.fetched) + '; live ADS-B takes precedence.' : ''));
    drawMap(fl, a);
  }

  /*
   * Route map in Web Mercator over NASA GIBS tiles: Blue Marble shaded
   * relief with the reference borders and coastlines on top.  Public-domain
   * imagery that needs no key.  Framed on the two airports so the tiles
   * stay put while the aircraft moves; they are only rebuilt when the frame
   * changes.  Blue Marble stops at zoom 8, which is plenty for a route.
   */
  var GIBS = 'https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/';
  var MAPW = 600, MAPH = 280, TILE = 256, NS = 'http://www.w3.org/2000/svg';
  function merc(p, z) {
    var n = TILE * Math.pow(2, z), la = Math.max(-85, Math.min(85, p[0])) * Math.PI / 180;
    return [(p[1] + 180) / 360 * n, (1 - Math.log(Math.tan(la) + 1 / Math.cos(la)) / Math.PI) / 2 * n];
  }
  function svgEl(parent, n, at, txt) {
    var e = document.createElementNS(NS, n); for (var q in at) e.setAttribute(q, at[q]);
    if (txt) e.textContent = txt; parent.appendChild(e); return e;
  }
  function drawMap(fl, a) {
    var svg = $('fmap'); if (!svg) return;
    var frame = [], pts = [];
    function ok(p) { return p && isFinite(p[0]) && isFinite(p[1]); }
    [fl.origin, fl.destination].forEach(function (p) { if (ok(p)) frame.push(p); });
    fl.trail.forEach(function (p) { if (ok(p)) pts.push(p); }); if (a) pts.push([a.lat, a.lon]);
    /* Frame on the airports; take the track in only if it strays outside. */
    var fit = frame.concat(pts);
    svg.style.display = fit.length ? '' : 'none'; if (!fit.length) return;
    var z = 8, x0, x1, y0, y1;
    for (; z > 1; z--) {
      x0 = y0 = Infinity; x1 = y1 = -Infinity;
      fit.forEach(function (p) { var m = merc(p, z); x0 = Math.min(x0, m[0]); x1 = Math.max(x1, m[0]); y0 = Math.min(y0, m[1]); y1 = Math.max(y1, m[1]); });
      if (x1 - x0 <= MAPW - 90 && y1 - y0 <= MAPH - 70) break;
    }
    var ox = Math.round((x0 + x1) / 2 - MAPW / 2), oy = Math.round((y0 + y1) / 2 - MAPH / 2);
    function xy(p) { var m = merc(p, z); return [(m[0] - ox).toFixed(1), (m[1] - oy).toFixed(1)]; }
    var key = z + '/' + ox + '/' + oy, tiles = svg.querySelector('g.tiles'), over = svg.querySelector('g.over');
    if (!tiles) { tiles = svgEl(svg, 'g', { 'class': 'tiles' }); over = svgEl(svg, 'g', { 'class': 'over' }); }
    if (tiles.getAttribute('data-key') !== key) {
      tiles.setAttribute('data-key', key);
      while (tiles.firstChild) tiles.removeChild(tiles.firstChild);
      var n = Math.pow(2, z);
      for (var ty = Math.floor(oy / TILE); ty * TILE < oy + MAPH; ty++) {
        if (ty < 0 || ty >= n) continue;
        for (var tx = Math.floor(ox / TILE); tx * TILE < ox + MAPW; tx++) {
          var wx = ((tx % n) + n) % n;
          var at = { x: tx * TILE - ox, y: ty * TILE - oy, width: TILE, height: TILE }, tail = z + '/' + ty + '/' + wx;
          at.href = GIBS + 'BlueMarble_ShadedRelief_Bathymetry/default/GoogleMapsCompatible_Level8/' + tail + '.jpeg';
          svgEl(tiles, 'image', at);
          at.href = GIBS + 'Reference_Features_15m/default/GoogleMapsCompatible_Level13/' + tail + '.png';
          at['class'] = 'ref'; svgEl(tiles, 'image', at);
        }
      }
    }
    while (over.firstChild) over.removeChild(over.firstChild);
    if (fl.origin && fl.destination) {
      /* Planned route: the great circle between the airports. */
      function v3(p) { var la = p[0] * Math.PI / 180, lo = p[1] * Math.PI / 180; return [Math.cos(la) * Math.cos(lo), Math.cos(la) * Math.sin(lo), Math.sin(la)]; }
      var va = v3(fl.origin), vb = v3(fl.destination), om = Math.acos(Math.min(1, va[0] * vb[0] + va[1] * vb[1] + va[2] * vb[2])), gc = [];
      for (var i = 0; i <= 48; i++) {
        var tt = i / 48, sa = om ? Math.sin((1 - tt) * om) / Math.sin(om) : 1 - tt, sb = om ? Math.sin(tt * om) / Math.sin(om) : tt;
        var v = [sa * va[0] + sb * vb[0], sa * va[1] + sb * vb[1], sa * va[2] + sb * vb[2]];
        gc.push(xy([Math.atan2(v[2], Math.hypot(v[0], v[1])) * 180 / Math.PI, Math.atan2(v[1], v[0]) * 180 / Math.PI]).join(','));
      }
      svgEl(over, 'polyline', { points: gc.join(' '), 'class': 'gc' });
    }
    if (pts.length > 1) {
      var line = pts.map(function (p) { return xy(p).join(','); }).join(' ');
      svgEl(over, 'polyline', { points: line, 'class': 'trailcase' });
      svgEl(over, 'polyline', { points: line, 'class': 'trail' });
    }
    [[fl.origin, fl.route.from], [fl.destination, fl.route.to]].forEach(function (ap) {
      if (!ok(ap[0])) return; var q = xy(ap[0]); svgEl(over, 'circle', { cx: q[0], cy: q[1], r: 6, 'class': 'apt' });
      /* Labels on the right half sit left of the dot so they stay inside. */
      var right = +q[0] > MAPW / 2;
      if (ap[1]) svgEl(over, 'text', { x: +q[0] + (right ? -10 : 10), y: +q[1] - 9, 'text-anchor': right ? 'end' : 'start' }, ap[1]);
    });
    var hp = xy(fl.home); if (hp[0] > 0 && hp[0] < MAPW && hp[1] > 0 && hp[1] < MAPH) svgEl(over, 'circle', { cx: hp[0], cy: hp[1], r: 4.5, 'class': 'home' });
    if (a) {
      var q2 = xy([a.lat, a.lon]), gp = svgEl(over, 'g', { transform: 'translate(' + q2[0] + ' ' + q2[1] + ') rotate(' + (a.track_valid ? a.track_deg : 0) + ') translate(-12 -12)', 'class': 'me' });
      svgEl(gp, 'path', { d: 'M21 16v-2l-8-5V3.5a1.5 1.5 0 0 0-3 0V9l-8 5v2l8-2.5V19l-2 1.5V22l3.5-1 3.5 1v-1.5L13 19v-5.5z' });
    }
  }
  function loadFlight() { get('/api/v1/flight', function (fj) { lastFlight = fj; quotaRender(fj.quota); if (focus) renderFlight(); }); }
  function temp(c, u) { return (u === 'f' ? c * 9 / 5 + 32 : c).toFixed(1) + (u === 'f' ? '°F' : '°C'); }
  function st(j) {
    t('ssid', j.ssid); t('rssi', j.rssi_dbm === null ? 'unavailable' : j.rssi_dbm + ' dBm');
    /* Climate chip and System row; absent on boards with no sensor. */
    if (typeof j.temperature_c === 'number') {
      t('climate', temp(j.temperature_c, j.temperature_unit));
      t('humidity', Math.round(j.humidity_percent) + '%');
      t('env', temp(j.temperature_c, j.temperature_unit) + ' · ' + Math.round(j.humidity_percent) + '% RH');
    }
    /* Power chip. On USB the charger holds the rail near full, so the level
       is only meaningful once running on the cell. */
    if (typeof j.battery_volts === 'number') {
      t('power', j.usb_present ? 'USB power' : j.battery_percent + '% · ' + j.battery_volts.toFixed(2) + ' V');
      t('power2', (j.usb_present ? 'USB' : 'Battery') + ' · ' + j.battery_volts.toFixed(2) + ' V');
    }
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
  var ticks = 0;
  function tick() {
    get('/api/v1/aircraft', air); get('/api/v1/status', st);
    if (focus || ticks % 15 === 0) loadFlight();
    ticks++;
  }

  /* ---- Flight number check ----
     Typing gets an instant format reading.  Airline numbers are then
     confirmed with adsbdb through the device (free); the Check button also
     asks Flystack for the schedule, which spends one lookup. */
  var fin = $('focus'), fval = $('fval'), fbtn = $('fcheck'), ftimer = null, fpoll = null, keySet = false;
  function norm(v) { return (v || '').toUpperCase().replace(/\s+/g, ''); }
  function classify(v) {
    if (!v) return { cls: '', msg: '' };
    if (/[^A-Z0-9~-]/.test(v)) return { cls: 'bad', msg: 'Use letters, digits and dashes only.' };
    if (v.length > 8) return { cls: 'bad', msg: 'At most 8 characters.' };
    var z = v.match(/^([A-Z]{3})0+([1-9][0-9A-Z]*)$/);
    if (z) return { cls: 'warn', msg: 'Callsigns are broadcast without leading zeros.', fix: z[1] + z[2] };
    if (/^[A-Z]{3}\d[0-9A-Z]{0,3}$/.test(v)) return { cls: '', msg: 'Airline callsign ' + v + ' — confirming the number…', kind: 'airline' };
    if (/-/.test(v) || /^N[1-9][0-9A-Z]{0,4}$/.test(v)) return { cls: 'ok', msg: 'Registration ' + v + ' — follows this airframe on whatever flight it flies.' };
    var hex = /^~?[0-9A-F]{6}$/.test(v), iata = /^([A-Z]\d|\d[A-Z]|[A-Z]{2})\d{1,4}[A-Z]?$/.test(v);
    /* "AA1234" is both a valid address and American's flight 1234. */
    if (hex && iata) return { cls: '', msg: v + ' could be an ICAO address or an IATA flight number — checking…', kind: 'iata', ambiguous: true };
    if (hex) return { cls: 'ok', msg: 'ICAO address ' + v + ' — follows this airframe on whatever flight it flies.' };
    if (iata) return { cls: 'warn', msg: v + ' looks like the IATA number printed on tickets; aircraft broadcast the ICAO callsign. Looking it up…', kind: 'iata' };
    return { cls: '', msg: 'Followed as a callsign or registration.' };
  }
  function showCheck(cls, nodes) {
    if (!fval) return;
    while (fval.firstChild) fval.removeChild(fval.firstChild);
    fval.className = 'fval ' + (cls || ''); fval.hidden = !nodes.length;
    nodes.forEach(function (n) { fval.appendChild(typeof n === 'string' ? document.createTextNode(n) : n); });
  }
  function bold(text) { var b = document.createElement('b'); b.textContent = text; return b; }
  function useButton(code) {
    var b = document.createElement('button'); b.type = 'button'; b.className = 'ghost'; b.textContent = 'Use ' + code;
    b.onclick = function () { fin.value = code; onFocusInput(); }; return b;
  }
  function onFocusInput() {
    var v = norm(fin.value), c = classify(v);
    if (fpoll) { clearInterval(fpoll); fpoll = null; }
    if (ftimer) { clearTimeout(ftimer); ftimer = null; }
    var nodes = c.msg ? [c.msg] : [];
    if (c.fix) { nodes.push(' '); nodes.push(useButton(c.fix)); }
    showCheck(c.cls, nodes);
    if (c.kind) ftimer = setTimeout(function () { runCheck(v, false); }, 700);
  }
  function runCheck(v, schedule) {
    var f2 = csrfForm('/api/v1/flight/check');
    [['code', v], ['schedule', schedule ? '1' : '0']].forEach(function (kv) { var i = document.createElement('input'); i.name = kv[0]; i.value = kv[1]; f2.appendChild(i); });
    if (schedule) showCheck('', ['Checking ' + v + ' with adsbdb and Flystack…']);
    post(f2, function (r) {
      if (!r.ok) { showCheck('bad', ['Check failed: ' + (r.j.error || 'rejected')]); return; }
      var tries = 0;
      if (fpoll) clearInterval(fpoll);
      fpoll = setInterval(function () {
        if (++tries > 45) { clearInterval(fpoll); fpoll = null; showCheck('warn', ['The device did not answer in time; try again.']); return; }
        get('/api/v1/flight/check', function (c) {
          if (c.state !== 'done' || c.query !== v || !fpoll) return;
          clearInterval(fpoll); fpoll = null; checkResult(c, v);
        });
      }, 800);
    });
  }
  function checkResult(c, v) {
    var r = c.route, nodes = [], cls = 'ok';
    if (!c.route_answered) { cls = 'warn'; nodes.push('Could not reach adsbdb to confirm ' + v + '. You can still save it.'); }
    else if (!r.valid && classify(v).ambiguous) nodes.push('No flight ' + v + ' is known, so it will be followed as the ICAO address ' + v + '.');
    else if (!r.valid) {
      cls = 'warn';
      nodes.push(bold(v)); nodes.push(' is not a flight number adsbdb knows. Check the digits — it will still be followed if an aircraft broadcasts it.');
    } else {
      nodes.push('✓ '); nodes.push(bold(r.callsign_icao || v));
      nodes.push(' · ' + parts(r.airline, r.callsign_iata, r.from && r.to ? r.from + ' → ' + r.to : ''));
      if (r.callsign_icao && r.callsign_icao !== v) {
        cls = 'warn'; nodes.push(document.createElement('br'));
        nodes.push('Aircraft broadcast '); nodes.push(bold(r.callsign_icao)); nodes.push(', not ' + v + '.'); nodes.push(useButton(r.callsign_icao));
      }
    }
    if (c.schedule_requested) {
      nodes.push(document.createElement('br'));
      var s = c.schedule;
      if (c.schedule_state === 'ok' && s) {
        nodes.push(parts('Flystack: ' + (s.status || 'found'), s.dep_time ? 'departs ' + hm(s.dep_time) + delayText(s.dep_delay_min) : '',
                         s.arr_time ? 'arrives ' + hm(s.arr_time) : '', s.aircraft));
      } else nodes.push(NOTE[c.schedule_state] || ('Flystack: ' + c.schedule_state));
      loadFlight();
    }
    showCheck(cls, nodes);
  }
  if (fin) {
    fin.oninput = onFocusInput;
    if (fin.value) onFocusInput();
  }
  if (fbtn) fbtn.onclick = function () {
    var v = norm(fin.value), c = classify(v);
    if (!v || c.cls === 'bad') { onFocusInput(); return; }
    if (ftimer) { clearTimeout(ftimer); ftimer = null; }
    runCheck(v, keySet && (c.kind === 'airline' || c.kind === 'iata'));
  };

  /* ---- Flystack key ---- */
  function quotaRender(q) {
    if (!q) return;
    keySet = q.key_set;
    t('fskey', q.key_set ? 'Key saved · ends in ' + q.key_hint : 'No key saved — scheduled times, delays and gates are off.');
    var rm = $('fskeyclear'); if (rm) rm.hidden = !q.key_set;
    if (fbtn) fbtn.title = q.key_set ? 'Confirms the number (free) and fetches its schedule (1 Flystack lookup)' : 'Confirms the number with adsbdb (free)';
    t('fsquota', !q.key_set ? '' : parts(
      q.remaining === null ? 'Remaining quota not known yet' : q.remaining + ' Flystack requests left' + (q.renewal ? ' (renews ' + q.renewal + ')' : ''),
      q.calls_today + ' of ' + q.daily_cap + ' used by this device in the last 24 h',
      q.last_error === 'unauthorized' ? 'Last lookup was refused (401/403)' : q.last_error === 'quota' ? 'Last lookup hit the quota' : ''));
  }
  function saveKey(value) {
    var f2 = csrfForm('/api/v1/flystack'), i = document.createElement('input'); i.name = 'key'; i.value = value; f2.appendChild(i);
    post(f2, function (r) {
      toast(r.ok ? (value ? 'Flystack key saved' : 'Flystack key removed') : 'Key not saved: ' + (r.j.error || 'rejected'), r.ok ? 'ok' : 'bad');
      if (r.ok) { var k = $('fskeyin'); if (k) k.value = ''; setTimeout(loadFlight, 1500); }
    });
  }
  var ks = $('fskeysave'); if (ks) ks.onclick = function () { var k = $('fskeyin'); if (k && k.value.trim()) saveKey(k.value.trim()); };
  var kc = $('fskeyclear'); if (kc) kc.onclick = function () { if (confirm('Remove the Flystack key from this device?')) saveKey(''); };

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
