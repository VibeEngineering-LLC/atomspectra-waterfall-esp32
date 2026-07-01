#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
#REC-11 pull-модель: PC-клиент забирает завершённые сегменты водопада с платы.

БЕЗОПАСНОСТЬ (зачем pull, а не push): все соединения — ИСХОДЯЩИЕ с ПК. На рабочем
компе НЕ открывается ни одного входящего порта и не создаётся ни одного правила
firewall. Плата ничего не инициирует; ПК сам опрашивает её и забирает данные.

Цикл одного прохода:
  1. GET  /api/csrf-token                       -> токен для мутирующего запроса
  2. GET  /api/waterfall/segments               -> список сегментов (JSON)
  3. для каждого finalized-сегмента:
       GET  /api/waterfall/segment?name=...      -> сырой .aswf, пишем атомарно
       сверяем размер с листингом; при совпадении:
       POST /api/waterfall/segment/delete?name=  -> плата стирает сегмент с Flash
     (X-CSRF-Token в заголовке; удаляем ТОЛЬКО после успешной верификации приёма)

Только стандартная библиотека (urllib). mDNS-хост atomspectra.local обходит
VPN-прокси, который может отдавать 503 на прямых LAN-адресах.

Примеры:
  python wf_pull_client.py --once
  python wf_pull_client.py --interval 120 --out D:\wf_segments
  python wf_pull_client.py --host http://atomspectra.local
"""
import argparse
import json
import os
import sys
import time
import urllib.request
import urllib.error

sys.stdout.reconfigure(encoding="utf-8")

HTTP_TIMEOUT = 30  # сек на запрос; сегмент ~1 МБ по LAN укладывается с запасом


def http_get(url, headers=None, binary=False):
    req = urllib.request.Request(url, headers=headers or {}, method="GET")
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
        data = r.read()
    return data if binary else data.decode("utf-8")


def http_post(url, headers=None):
    # тело пустое: имя сегмента идёт в query-строке (симметрично GET /segment)
    req = urllib.request.Request(url, data=b"", headers=headers or {}, method="POST")
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
        return r.status, r.read().decode("utf-8", "replace")


def get_csrf(host):
    tok = json.loads(http_get(host + "/api/csrf-token")).get("token", "")
    if len(tok) != 32:
        raise RuntimeError(f"плата вернула CSRF-токен неожиданной длины: {len(tok)}")
    return tok


def list_segments(host):
    return json.loads(http_get(host + "/api/waterfall/segments"))


def fetch_one(host, out_dir, seg, token):
    """Скачать один сегмент атомарно + подтвердить приём удалением на плате.
    Возвращает 'ok' | 'skip' | 'sizemismatch' | 'error:<...>'."""
    name = seg["name"]
    want = int(seg["bytes"])
    dst = os.path.join(out_dir, name)

    # идемпотентность: если файл уже забран целиком, повторно не качаем, но ack шлём
    have = os.path.getsize(dst) if os.path.exists(dst) else -1
    if have != want:
        try:
            blob = http_get(host + "/api/waterfall/segment?name=" + name, binary=True)
        except (urllib.error.URLError, urllib.error.HTTPError) as e:
            return f"error:get:{e}"
        if len(blob) != want:
            return "sizemismatch"                 # не удаляем на плате — заберём позже
        tmp = dst + ".part"
        with open(tmp, "wb") as f:
            f.write(blob)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, dst)                      # атомарная публикация

    # приём подтверждён (файл на диске == листинг) -> плата освобождает Flash
    try:
        st, _ = http_post(host + "/api/waterfall/segment/delete?name=" + name,
                          headers={"X-CSRF-Token": token})
    except (urllib.error.URLError, urllib.error.HTTPError) as e:
        return f"error:del:{e}"
    return "ok" if st == 200 else f"error:del-status:{st}"


def one_pass(host, out_dir):
    token = get_csrf(host)
    segs = list_segments(host)
    pending = [s for s in segs if s.get("finalized")]
    got = skipped = failed = 0
    for seg in pending:
        r = fetch_one(host, out_dir, seg, token)
        if r == "ok":
            got += 1
            print(f"  ✓ {seg['name']}  {seg['bytes']} B  забран+стёрт на плате")
        elif r == "sizemismatch":
            skipped += 1
            print(f"  ~ {seg['name']}  размер не сошёлся — повтор в след. проходе")
        else:
            failed += 1
            print(f"  ✗ {seg['name']}  {r}")
    open_cnt = sum(1 for s in segs if not s.get("finalized"))
    print(f"проход: забрано {got}, отложено {skipped}, ошибок {failed}, "
          f"открытых (пропущены) {open_cnt}")
    return got, failed


def main():
    ap = argparse.ArgumentParser(description="Pull-клиент сегментов водопада (#REC-11)")
    ap.add_argument("--host", default="http://atomspectra.local",
                    help="базовый URL платы (default: mDNS, обходит VPN-прокси)")
    ap.add_argument("--out", default=None,
                    help="папка для .aswf (default: <script>/../received)")
    ap.add_argument("--interval", type=int, default=60,
                    help="секунд между проходами (default 60)")
    ap.add_argument("--once", action="store_true", help="один проход и выход")
    args = ap.parse_args()

    host = args.host.rstrip("/")
    out_dir = args.out or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                       "..", "received")
    out_dir = os.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    print(f"host={host}  out={out_dir}  interval={args.interval}s  once={args.once}")

    while True:
        try:
            one_pass(host, out_dir)
        except (urllib.error.URLError, urllib.error.HTTPError, RuntimeError) as e:
            print(f"проход не удался: {e}")
        if args.once:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
