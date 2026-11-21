/* Phone Lab telemetry (development only; injected by the phone-lab dev server, never part of the production bundle).
 * Sends device info, errors, console warnings, frame-time statistics and XR session events to /lab/events. */
(function () {
  if (window.__phoneLab) return;
  var sessionId = Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
  var queue = [];
  var seq = 0;
  var MAX_QUEUE = 400;
  var consoleBudget = 60;

  function emit(type, data) {
    if (queue.length >= MAX_QUEUE) queue.shift();
    queue.push({ type: type, seq: seq++, t: Math.round(performance.now()), wall: Date.now(), session: sessionId, data: data || {} });
  }
  function flush(useBeacon) {
    if (queue.length === 0) return;
    var batch = queue.splice(0, 200);
    var body = JSON.stringify({ events: batch });
    try {
      if (useBeacon && navigator.sendBeacon) { navigator.sendBeacon('/lab/events', new Blob([body], { type: 'application/json' })); return; }
      fetch('/lab/events', { method: 'POST', headers: { 'content-type': 'application/json' }, body: body, keepalive: true }).catch(function () {});
    } catch (e) { /* telemetry must never break the app */ }
  }
  setInterval(function () { flush(false); }, 2000);
  document.addEventListener('visibilitychange', function () { emit('VISIBILITY', { state: document.visibilityState }); if (document.visibilityState === 'hidden') flush(true); });
  window.addEventListener('pagehide', function () { emit('PAGEHIDE'); flush(true); });

  window.addEventListener('error', function (e) {
    emit('JS_ERROR', { message: String(e.message), source: e.filename, line: e.lineno, col: e.colno, stack: e.error && e.error.stack ? String(e.error.stack).slice(0, 1500) : null });
  });
  window.addEventListener('unhandledrejection', function (e) {
    var r = e.reason;
    emit('JS_REJECTION', { message: r && r.message ? String(r.message) : String(r), stack: r && r.stack ? String(r.stack).slice(0, 1500) : null });
  });
  ['warn', 'error'].forEach(function (level) {
    var original = console[level];
    console[level] = function () {
      if (consoleBudget > 0) {
        consoleBudget -= 1;
        try { emit('CONSOLE_' + level.toUpperCase(), { text: Array.prototype.slice.call(arguments).map(function (a) { return typeof a === 'string' ? a : (a && a.message) || JSON.stringify(a); }).join(' ').slice(0, 600) }); } catch (e) { /* ignore */ }
      }
      return original.apply(console, arguments);
    };
  });
  setInterval(function () { consoleBudget = Math.min(60, consoleBudget + 20); }, 10000);

  function percentile(sorted, p) { if (sorted.length === 0) return null; return sorted[Math.min(sorted.length - 1, Math.ceil(p / 100 * sorted.length) - 1)]; }
  function FrameStats(kind) {
    this.kind = kind; this.last = 0; this.deltas = []; this.long33 = 0; this.long50 = 0; this.total = 0; this.timer = setInterval(this.report.bind(this), 5000);
  }
  FrameStats.prototype.tick = function (now) {
    if (this.last > 0) {
      var d = now - this.last;
      if (d < 1000) { this.deltas.push(d); this.total += 1; if (d > 33.4) this.long33 += 1; if (d > 50) this.long50 += 1; }
    }
    this.last = now;
  };
  FrameStats.prototype.report = function () {
    if (this.deltas.length === 0) return;
    var s = this.deltas.slice().sort(function (a, b) { return a - b; });
    var mem = performance.memory ? performance.memory.usedJSHeapSize : null;
    emit('FRAME_STATS', { kind: this.kind, frames: this.total, fps: Math.round(1000 / (s.reduce(function (a, b) { return a + b; }, 0) / s.length) * 10) / 10,
      p50: Math.round(percentile(s, 50) * 10) / 10, p95: Math.round(percentile(s, 95) * 10) / 10, over33: this.long33, over50: this.long50, heapBytes: mem });
    this.deltas = [];
  };
  var pageStats = new FrameStats('page');
  (function loop(now) { pageStats.tick(now); requestAnimationFrame(loop); })(performance.now());

  try {
    var longTasks = 0; var longMs = 0;
    new PerformanceObserver(function (list) { list.getEntries().forEach(function (entry) { longTasks += 1; longMs += entry.duration; }); }).observe({ entryTypes: ['longtask'] });
    setInterval(function () { if (longTasks > 0) { emit('LONG_TASKS', { count: longTasks, totalMs: Math.round(longMs) }); longTasks = 0; longMs = 0; } }, 5000);
  } catch (e) { /* longtask unsupported */ }

  // XR: log session lifecycle and measure XR frame pacing (the same numbers the performance acceptance criteria use).
  if (navigator.xr && navigator.xr.requestSession) {
    var originalRequest = navigator.xr.requestSession.bind(navigator.xr);
    navigator.xr.requestSession = function (mode, init) {
      emit('XR_SESSION_REQUEST', { mode: mode, features: init && { required: init.requiredFeatures, optional: init.optionalFeatures } });
      return originalRequest(mode, init).then(function (session) {
        var stats = new FrameStats('xr');
        emit('XR_SESSION_STARTED', { mode: mode, blend: session.environmentBlendMode, depth: session.depthUsage || null, format: session.depthDataFormat || null });
        var originalRaf = session.requestAnimationFrame.bind(session);
        session.requestAnimationFrame = function (callback) { return originalRaf(function (time, frame) { stats.tick(time); callback(time, frame); }); };
        session.addEventListener('end', function () { stats.report(); clearInterval(stats.timer); emit('XR_SESSION_ENDED', {}); flush(false); });
        return session;
      }, function (error) { emit('XR_SESSION_FAILED', { name: error && error.name, message: error && error.message }); throw error; });
    };
  }

  function deviceInfo() {
    var gl = null; var renderer = null;
    try { gl = document.createElement('canvas').getContext('webgl2'); var ext = gl && gl.getExtension('WEBGL_debug_renderer_info'); renderer = ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : null; } catch (e) { /* ignore */ }
    var info = { userAgent: navigator.userAgent, secure: window.isSecureContext, screen: [screen.width, screen.height], dpr: window.devicePixelRatio, cores: navigator.hardwareConcurrency,
      deviceMemoryGb: navigator.deviceMemory || null, webgl2: !!gl, gpu: renderer, xrApi: !!navigator.xr, mediaDevices: !!navigator.mediaDevices, connection: navigator.connection ? { type: navigator.connection.effectiveType, rtt: navigator.connection.rtt } : null, page: location.href };
    emit('DEVICE_INFO', info);
    if (navigator.xr && navigator.xr.isSessionSupported) navigator.xr.isSessionSupported('immersive-ar').then(function (ok) { emit('XR_SUPPORT', { immersiveAr: ok }); }, function (e) { emit('XR_SUPPORT', { error: String(e) }); });
    if (navigator.getBattery) navigator.getBattery().then(function (b) { emit('BATTERY', { level: b.level, charging: b.charging }); }, function () {});
  }
  deviceInfo();


  window.__phoneLab = { sessionId: sessionId, emit: emit, flush: function () { flush(false); } };
})();
