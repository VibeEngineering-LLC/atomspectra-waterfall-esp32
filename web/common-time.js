// #FIELD-5: авто-синхронизация времени платы от браузера телефона.
// В полевом AP (Outdoor) интернета нет → SNTP не работает → плата стартует с
// near-epoch (1970). Любая открытая страница шлёт текущее время браузера на
// POST /api/time; прошивка сама решает, принимать ли (net_time_should_accept):
// при активном SNTP — откажет (война источников), иначе примет при расхождении
// > 5 с. manual=false → авто-режим (для явного ручного ввода — форма в /system).
(function () {
  async function syncTime() {
    try {
      const t = await fetch('/api/csrf-token').then(r => r.json());
      const res = await fetch('/api/time', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': t.token },
        body: JSON.stringify({ epoch_ms: Date.now(), manual: false })
      }).then(r => r.json());
      if (res && res.accepted)
        console.log('[time] синхронизировано от браузера, источник=' + res.source);
    } catch (e) {
      // Оффлайн или рано при загрузке — молча, авто-синк не должен мешать UI.
    }
  }
  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', syncTime);
  else
    syncTime();
  // Повтор раз в час с открытой вкладки: корректирует дрейф часов платы (нет RTC,
  // нет SNTP в поле) без перезагрузки страницы. Приём — по той же guard-логике.
  setInterval(syncTime, 3600000);
})();

/* #PERF-2: shared HEAVY fetch — retries on 503 + Retry-After (segment/export/window). */
window.heavyFetch = async function heavyFetch(url, opts) {
  opts = opts || {};
  var maxAttempts = opts.maxAttempts || 30;
  for (var i = 0; i < maxAttempts; i++) {
    var r = await fetch(url, opts);
    if (r.status !== 503) return r;
    var ra = parseInt(r.headers.get("Retry-After") || "1", 10);
    if (!(ra > 0)) ra = 1;
    await new Promise(function (res) { setTimeout(res, ra * 1000); });
  }
  throw new Error("heavyFetch: still busy after " + maxAttempts + " attempts: " + url);
};
