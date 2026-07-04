#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
#REC-12: рекордер спектрограмм AtomSpectra — программа с UI поверх pull-клиента.

Окно (tkinter, stdlib, зависимостей нет): адрес платы, файл записи, интервал
опроса, Старт/Стоп, счётчики (строк в файле, длительность, температура,
сегментов на плате) и журнал. Логика забора/шва — целиком из wf_pull_client.py
(та же, что доказана на железе): сегменты платы -> единый .aswf + temps.csv,
delete на плате только после fsync на диск.

Запуск: wf_recorder.bat (двойной клик) или
  python wf_recorder_app.py [--host http://IP] [--stitch FILE] [--interval 60]

Служебные флаги самопроверки (без UI-кликов):
  --autostart          сразу запустить опрос
  --exit-after N       выйти через N секунд (для smoke-теста)
"""
import argparse
import contextlib
import io
import json
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

if sys.stdout is not None:
    sys.stdout.reconfigure(encoding="utf-8")
if sys.stderr is not None:
    sys.stderr.reconfigure(encoding="utf-8")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wf_pull_client as wpc  # noqa: E402  (Stitcher, get_csrf, list_segments, ...)

DEF_HOST = "http://atomspectra.local"
DEF_INTERVAL = 60


class RecorderCore:
    """Фоновый поток опроса. UI читает события из self.events (queue)."""

    def __init__(self, host, stitch_path, interval):
        self.host = host.rstrip("/")
        self.interval = interval
        self.stitcher = wpc.Stitcher(stitch_path)
        self.events = queue.Queue()
        self._stop = threading.Event()
        self._thread = None
        self.last_temp = None
        self.board = {}          # последний /api/waterfall/status
        self.last_pass = None    # время последнего успешного прохода

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    @property
    def running(self):
        return self._thread is not None and self._thread.is_alive() and not self._stop.is_set()

    def _log(self, msg):
        self.events.put(("log", time.strftime("%H:%M:%S") + "  " + msg))

    def _loop(self):
        self._log(f"старт: плата {self.host}, файл {self.stitcher.path}, "
                  f"интервал {self.interval} с")
        while not self._stop.is_set():
            try:
                self._one_pass()
            except Exception as e:  # сеть/плата недоступны — ждём следующий проход
                self._log(f"проход не удался: {e}")
                self.board = {}   # сбросить устаревший статус платы → UI покажет «нет связи»
            self.events.put(("status", None))
            self._stop.wait(self.interval)
        self._log("остановлено")
        self.events.put(("status", None))

    def _one_pass(self):
        token = wpc.get_csrf(self.host)
        t = self.stitcher.log_temps(self.host)
        if t:
            self.last_temp = t
        try:
            self.board = json.loads(wpc.http_get(self.host + "/api/waterfall/status"))
        except Exception:
            self.board = {}
        segs = wpc.list_segments(self.host)
        pending = sorted([s for s in segs if s.get("finalized")],
                         key=lambda s: int(s["idx"]))
        for seg in pending:
            if self._stop.is_set():
                return
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):   # предупреждения клиента -> журнал
                r, rows, gap = wpc.fetch_one_stitch(self.host, self.stitcher, seg, token)
            for line in buf.getvalue().splitlines():
                self._log(line.strip())
            if r == "ok" and rows:
                self._log(f"✓ {seg['name']} +{rows} строк, стёрт на плате")
            elif r == "ok":
                self._log(f"✓ {seg['name']} уже был вшит, только ack")
            elif r == "sizemismatch":
                self._log(f"~ {seg['name']} размер не сошёлся, повтор позже")
            else:
                self._log(f"✗ {seg['name']} {r}")
            if gap is not None:
                self._log(f"⚠ пауза платы перед {seg['name']}: {gap:+.0f} с")
        self.last_pass = time.time()


class RecorderUI:
    def __init__(self, root, args):
        self.root = root
        root.title("AtomSpectra — запись спектрограммы (#REC-12)")
        root.minsize(640, 420)

        frm = ttk.Frame(root, padding=8)
        frm.pack(fill="both", expand=True)

        # --- параметры ---
        row = ttk.Frame(frm)
        row.pack(fill="x")
        ttk.Label(row, text="Плата:").pack(side="left")
        self.host_var = tk.StringVar(value=args.host)
        ttk.Entry(row, textvariable=self.host_var, width=28).pack(side="left", padx=4)
        ttk.Label(row, text="Опрос, с:").pack(side="left", padx=(12, 0))
        self.int_var = tk.StringVar(value=str(args.interval))
        ttk.Entry(row, textvariable=self.int_var, width=5).pack(side="left", padx=4)

        row2 = ttk.Frame(frm)
        row2.pack(fill="x", pady=(6, 0))
        ttk.Label(row2, text="Файл:").pack(side="left")
        self.file_var = tk.StringVar(value=args.stitch)
        ttk.Entry(row2, textvariable=self.file_var).pack(side="left", padx=4,
                                                         fill="x", expand=True)
        ttk.Button(row2, text="Выбрать…", command=self.pick_file).pack(side="left")

        # --- кнопки ---
        row3 = ttk.Frame(frm)
        row3.pack(fill="x", pady=8)
        self.btn = ttk.Button(row3, text="▶ Старт", command=self.toggle)
        self.btn.pack(side="left")
        ttk.Button(row3, text="Новый файл…",
                   command=self.new_file).pack(side="left", padx=8)
        ttk.Button(row3, text="Открыть папку",
                   command=self.open_folder).pack(side="left", padx=8)
        self.state_lbl = ttk.Label(row3, text="ожидание", foreground="gray")
        self.state_lbl.pack(side="left", padx=12)

        # --- счётчики ---
        self.stats = ttk.Label(frm, text="", justify="left")
        self.stats.pack(fill="x", pady=(0, 6))

        # --- журнал ---
        self.log = scrolledtext.ScrolledText(frm, height=14, state="disabled",
                                             font=("Consolas", 9))
        self.log.pack(fill="both", expand=True)

        self.core = None
        self.root.after(300, self.tick)
        if args.autostart:
            self.toggle()
        if args.exit_after:
            self.root.after(args.exit_after * 1000, self.root.destroy)

    # -------------------------------------------------------------- действия
    def pick_file(self):
        p = filedialog.asksaveasfilename(
            defaultextension=".aswf", filetypes=[("AtomSpectra waterfall", "*.aswf")],
            initialfile=os.path.basename(self.file_var.get()),
            initialdir=os.path.dirname(self.file_var.get()) or ".")
        if p:
            self.file_var.set(p)

    def new_file(self):
        """Начать запись в новый/очищенный файл (останавливает текущую запись,
        стирает файл+state.json+temps.csv по выбранному пути, если есть)."""
        if self.core and self.core.running:
            self.core.stop()
            self.btn.config(text="▶ Старт")
        p = filedialog.asksaveasfilename(
            title="Новый файл спектрограммы",
            defaultextension=".aswf", filetypes=[("AtomSpectra waterfall", "*.aswf")],
            initialfile=os.path.basename(self.file_var.get()),
            initialdir=os.path.dirname(self.file_var.get()) or ".")
        if not p:
            return
        for suffix in ("", ".state.json", ".temps.csv"):
            fp = p + suffix
            if os.path.exists(fp):
                try:
                    os.remove(fp)
                except OSError as e:
                    messagebox.showerror("Новый файл", f"Не удалось удалить {fp}:\n{e}")
                    return
        self.file_var.set(p)
        self.core = None
        self.stats.config(text="")
        self.log.config(state="normal")
        self.log.insert("end", time.strftime("%H:%M:%S")
                        + f"  новый файл: {p} (старые данные/state очищены, жми Старт)\n")
        self.log.see("end")
        self.log.config(state="disabled")

    def open_folder(self):
        d = os.path.dirname(os.path.abspath(self.file_var.get()))
        if os.path.isdir(d):
            subprocess.Popen(["explorer", d])

    def toggle(self):
        if self.core and self.core.running:
            self.core.stop()
            self.btn.config(text="▶ Старт")
            return
        try:
            interval = max(5, int(self.int_var.get()))
        except ValueError:
            interval = DEF_INTERVAL
        path = os.path.abspath(self.file_var.get())
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.core = RecorderCore(self.host_var.get(), path, interval)
        self.core.start()
        self.btn.config(text="⏸ Стоп")

    # -------------------------------------------------------------- обновление
    def tick(self):
        if self.core:
            try:
                while True:
                    kind, payload = self.core.events.get_nowait()
                    if kind == "log":
                        self.log.config(state="normal")
                        self.log.insert("end", payload + "\n")
                        self.log.see("end")
                        self.log.config(state="disabled")
            except queue.Empty:
                pass
            self.refresh_stats()
        self.root.after(500, self.tick)

    def refresh_stats(self):
        c = self.core
        st = c.stitcher.state
        rows = st.get("rows", 0)
        dur_h = st.get("dur_sum", 0) / 3600.0
        temp = f"{c.last_temp[0]}°C" if c.last_temp else "—"
        b = c.board
        board_txt = (f"запись {'ВКЛ' if b.get('recording') else 'выкл'}, "
                     f"сегментов {b.get('seg_count', '—')}, "
                     f"вытеснено {b.get('seg_dropped', '—')}") if b else "нет связи"
        lp = time.strftime("%H:%M:%S", time.localtime(c.last_pass)) if c.last_pass else "—"
        self.stats.config(text=(f"В файле: {rows} строк ({dur_h:.2f} ч)   "
                                f"Температура: {temp}\n"
                                f"Плата: {board_txt}   Последний проход: {lp}"))
        if c.running:
            self.state_lbl.config(text="ИДЁТ ЗАПИСЬ", foreground="green")
        else:
            self.state_lbl.config(text="остановлено", foreground="gray")


def _default_output_path():
    """Дефолтный путь для .aswf. При запуске из .py — рядом в ../received/;
    при frozen exe (PyInstaller) — в подкаталоге received/ рядом с exe."""
    if getattr(sys, "frozen", False):
        base = os.path.dirname(os.path.abspath(sys.executable))
        return os.path.abspath(os.path.join(base, "received", "spectrogram.aswf"))
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "received", "spectrogram.aswf"))


def main():
    ap = argparse.ArgumentParser(description="Рекордер спектрограмм AtomSpectra (#REC-12)")
    default_out = _default_output_path()
    ap.add_argument("--host", default=DEF_HOST)
    ap.add_argument("--stitch", default=default_out)
    ap.add_argument("--interval", type=int, default=DEF_INTERVAL)
    ap.add_argument("--autostart", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--exit-after", type=int, default=0, help=argparse.SUPPRESS)
    args = ap.parse_args()

    root = tk.Tk()
    RecorderUI(root, args)
    root.mainloop()


if __name__ == "__main__":
    main()
