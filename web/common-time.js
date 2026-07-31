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

/* #PERF-2: shared HEAVY fetch — retries on 503 + Retry-After (segment/export/window).
   Retry-After берётся с потолка сервера/прокси, поэтому доверяем ему ограниченно:
   clamp [1,5] с плюс общий дедлайн на всю серию попыток. Без дедлайна 30 попыток
   по 5 с растянулись бы на 150 с, и вкладка выглядела бы зависшей. */
window.HEAVY_RETRY_MIN_S = 1;
window.HEAVY_RETRY_MAX_S = 5;
window.HEAVY_DEADLINE_MS = 30000;

window.heavyFetch = async function heavyFetch(url, opts) {
  opts = opts || {};
  var maxAttempts = opts.maxAttempts || 30;
  var deadlineMs = opts.deadlineMs || window.HEAVY_DEADLINE_MS;
  var started = Date.now();
  for (var i = 0; i < maxAttempts; i++) {
    var r = await fetch(url, opts);
    if (r.status !== 503) return r;
    var ra = parseInt(r.headers.get("Retry-After") || "1", 10);
    if (!(ra > 0)) ra = window.HEAVY_RETRY_MIN_S;
    if (ra > window.HEAVY_RETRY_MAX_S) ra = window.HEAVY_RETRY_MAX_S;
    var left = deadlineMs - (Date.now() - started);
    if (left <= 0) break;
    await new Promise(function (res) { setTimeout(res, Math.min(ra * 1000, left)); });
  }
  throw new Error("heavyFetch: still busy after "
    + Math.round((Date.now() - started) / 1000) + " s / " + maxAttempts
    + " attempts: " + url);
};

/* #PERF-2: скачивание HEAVY-эндпоинта. Через location.href нельзя: при занятых
   воротах браузер уйдёт на страницу с 503-JSON вместо файла, и пользователь
   потеряет и выгрузку, и текущее состояние вкладки. Имя файла берём из
   Content-Disposition — там уже учтён пользовательский префикс выгрузок. */
window.heavyDownload = async function heavyDownload(url, fallbackName) {
  var r = await window.heavyFetch(url);
  if (!r.ok) throw new Error("HTTP " + r.status + ": " + url);
  var name = fallbackName || url.split("/").pop();
  var cd = r.headers.get("Content-Disposition") || "";
  /* RFC 6266: если присланы оба параметра, filename* побеждает голый filename.
     Прошивка сейчас отдаёт только ASCII (префикс выгрузок режется до
     [A-Za-z0-9_-] в boot_config.c), так что второй вариант и есть рабочий,
     но порядок проверки важен для любого другого источника заголовка. */
  var m = /filename\*=\s*(?:UTF-8'')?"?([^";]+)"?/i.exec(cd)
       || /filename=\s*"?([^";]+)"?/i.exec(cd);
  if (m) { try { name = decodeURIComponent(m[1]); } catch (e) { name = m[1]; } }
  var href = URL.createObjectURL(await r.blob());
  var a = document.createElement("a");
  a.href = href;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(function () { URL.revokeObjectURL(href); }, 10000);
  return name;
};
