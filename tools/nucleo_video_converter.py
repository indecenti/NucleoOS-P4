#!/usr/bin/env python3
"""
NucleoV2 Video Converter — turn any video into the MPEG-1 format the NucleoOS ESP32-P4 player
decodes smoothly.

The device has NO hardware video decoder except JPEG, so playback is software MPEG-1 (pl_mpeg). To
stay real-time on the 360 MHz RISC-V core the clip must be small and simple. This tool bakes in the
hard-won recipe:
  * MPEG-1 video, height 192 by default (keeps aspect ratio -> width computed, NO crop)
  * 24 fps (MPEG-1 only allows fixed rates; 24 is the lowest sane one)
  * NO B-frames  (-bf 0)  -> fewer reference frames, faster decode
  * MP2 audio, 48 kHz stereo  (48 kHz = the board's native sink rate; avoids the underrun stall)
  * MPEG-PS container (.mpg)

Needs ffmpeg + ffprobe on PATH (winget install Gyan.FFmpeg).
"""
import os
import re
import shutil
import subprocess
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# ---- locate ffmpeg / ffprobe -------------------------------------------------
def _find(tool):
    p = shutil.which(tool)
    if p:
        return p
    # common winget shim location
    cand = os.path.expandvars(rf"%LOCALAPPDATA%\Microsoft\WinGet\Links\{tool}.exe")
    return cand if os.path.exists(cand) else None

FFMPEG = _find("ffmpeg")
FFPROBE = _find("ffprobe")

# Height presets (all multiples of 16 -> clean MPEG-1 macroblocks). 192 = sweet spot: ~real-time on
# the P4 and still watchable. Bigger = sharper but risks slow-motion; smaller = faster.
HEIGHTS = [144, 160, 176, 192, 208, 240]
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


def probe(path):
    """Return (duration_s, vid_w, vid_h, [audio streams])."""
    out = subprocess.run(
        [FFPROBE, "-v", "error", "-show_entries",
         "format=duration:stream=index,codec_type,width,height,channels:stream_tags=language,title",
         "-of", "default=noprint_wrappers=1", path],
        capture_output=True, text=True, creationflags=CREATE_NO_WINDOW).stdout
    dur, vw, vh = 0.0, 0, 0
    audios, cur = [], {}
    def flush():
        if cur.get("codec_type") == "audio":
            audios.append(dict(cur))
    for line in out.splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k == "duration":
            try: dur = float(v)
            except ValueError: pass
        elif k == "index":
            flush(); cur.clear(); cur["index"] = v
        elif k in ("codec_type", "channels"):
            cur[k] = v
        elif k == "width" and cur.get("codec_type") is None:  # first stream width often = video
            pass
        elif k == "width":
            cur["width"] = v
        elif k == "height":
            cur["height"] = v
        elif k == "TAG:language":
            cur["lang"] = v
        elif k == "TAG:title":
            cur["title"] = v
        if cur.get("width") and cur.get("height") and vw == 0 and cur.get("codec_type") != "audio":
            try: vw, vh = int(cur["width"]), int(cur["height"])
            except ValueError: pass
    flush()
    # a cleaner second pass just for the video size (the above is best-effort)
    v = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0",
                        "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
                       capture_output=True, text=True, creationflags=CREATE_NO_WINDOW).stdout.strip()
    if "," in v:
        try: vw, vh = (int(x) for x in v.split(",")[:2])
        except ValueError: pass
    return dur, vw, vh, audios


def even16(n):
    n = int(round(n / 16.0)) * 16
    return max(16, n)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("NucleoV2 Video Converter — MPEG-1")
        self.geometry("640x560")
        self.resizable(False, True)
        self.proc = None
        self.dur = 0.0
        self.src_w = self.src_h = 0
        self.audios = []

        pad = dict(padx=10, pady=4)

        # input file
        f1 = ttk.Frame(self); f1.pack(fill="x", **pad)
        ttk.Label(f1, text="Video sorgente:").pack(anchor="w")
        r = ttk.Frame(f1); r.pack(fill="x")
        self.in_var = tk.StringVar()
        ttk.Entry(r, textvariable=self.in_var).pack(side="left", fill="x", expand=True)
        ttk.Button(r, text="Sfoglia…", command=self.pick_in).pack(side="left", padx=4)

        # source info
        self.info = ttk.Label(self, text="—", foreground="#555")
        self.info.pack(anchor="w", padx=10)

        # options
        opt = ttk.LabelFrame(self, text="Formato NucleoV2")
        opt.pack(fill="x", **pad)

        r = ttk.Frame(opt); r.pack(fill="x", padx=8, pady=4)
        ttk.Label(r, text="Altezza (p):").pack(side="left")
        self.h_var = tk.IntVar(value=192)
        self.h_box = ttk.Combobox(r, width=6, state="readonly",
                                  values=HEIGHTS, textvariable=self.h_var)
        self.h_box.pack(side="left", padx=6)
        self.h_box.bind("<<ComboboxSelected>>", lambda e: self.recalc())
        self.dim_lbl = ttk.Label(r, text="", foreground="#080")
        self.dim_lbl.pack(side="left", padx=10)

        r = ttk.Frame(opt); r.pack(fill="x", padx=8, pady=4)
        ttk.Label(r, text="Bitrate video (kbps):").pack(side="left")
        self.br_var = tk.IntVar(value=900)
        ttk.Spinbox(r, from_=200, to=3000, increment=50, width=7,
                    textvariable=self.br_var, command=self.recalc).pack(side="left", padx=6)
        self.size_lbl = ttk.Label(r, text="", foreground="#555")
        self.size_lbl.pack(side="left", padx=10)

        r = ttk.Frame(opt); r.pack(fill="x", padx=8, pady=4)
        ttk.Label(r, text="Traccia audio:").pack(side="left")
        self.a_var = tk.StringVar()
        self.a_box = ttk.Combobox(r, width=40, state="readonly", textvariable=self.a_var)
        self.a_box.pack(side="left", padx=6)

        # fixed-recipe note
        ttk.Label(opt, text="24 fps · no B-frame · MP2 48 kHz stereo · MPEG-PS  (aspetto preservato, nessun crop)",
                  foreground="#888").pack(anchor="w", padx=8, pady=(2, 6))

        # output
        f2 = ttk.Frame(self); f2.pack(fill="x", **pad)
        ttk.Label(f2, text="File di uscita (.mpg):").pack(anchor="w")
        r = ttk.Frame(f2); r.pack(fill="x")
        self.out_var = tk.StringVar()
        ttk.Entry(r, textvariable=self.out_var).pack(side="left", fill="x", expand=True)
        ttk.Button(r, text="Sfoglia…", command=self.pick_out).pack(side="left", padx=4)

        # progress + actions
        self.pb = ttk.Progressbar(self, mode="determinate", maximum=1000)
        self.pb.pack(fill="x", padx=10, pady=(8, 2))
        self.stat = ttk.Label(self, text="Pronto." if FFMPEG else "ERRORE: ffmpeg non trovato nel PATH.",
                              foreground="#080" if FFMPEG else "#c00")
        self.stat.pack(anchor="w", padx=10)

        r = ttk.Frame(self); r.pack(fill="x", **pad)
        self.go = ttk.Button(r, text="Converti", command=self.start)
        self.go.pack(side="left")
        self.cancel = ttk.Button(r, text="Annulla", command=self.stop, state="disabled")
        self.cancel.pack(side="left", padx=6)

        # log
        self.log = tk.Text(self, height=8, wrap="word", font=("Consolas", 8))
        self.log.pack(fill="both", expand=True, padx=10, pady=(4, 10))

        if not FFMPEG or not FFPROBE:
            self.go.config(state="disabled")

    # ---- UI helpers ----------------------------------------------------------
    def logln(self, s):
        self.log.insert("end", s + "\n"); self.log.see("end")

    def pick_in(self):
        p = filedialog.askopenfilename(
            title="Scegli un video",
            filetypes=[("Video", "*.mp4 *.mkv *.avi *.mov *.webm *.m4v *.ts *.wmv *.flv"), ("Tutti", "*.*")])
        if not p:
            return
        self.in_var.set(p)
        base = os.path.splitext(os.path.basename(p))[0]
        self.out_var.set(os.path.join(os.path.dirname(p), base + "_nucleo.mpg"))
        self.load_info(p)

    def pick_out(self):
        p = filedialog.asksaveasfilename(defaultextension=".mpg",
                                         filetypes=[("MPEG-PS", "*.mpg")])
        if p:
            self.out_var.set(p)

    def load_info(self, path):
        self.stat.config(text="Analisi…", foreground="#555"); self.update_idletasks()
        try:
            self.dur, self.src_w, self.src_h, self.audios = probe(path)
        except Exception as e:
            messagebox.showerror("ffprobe", str(e)); return
        mm = int(self.dur // 60); ss = int(self.dur % 60)
        self.info.config(text=f"Sorgente: {self.src_w}×{self.src_h}  ·  {mm}:{ss:02d}  ·  "
                              f"{len(self.audios)} traccia/e audio")
        # audio dropdown
        labels = []
        for i, a in enumerate(self.audios):
            lang = a.get("lang", "?"); title = a.get("title", ""); ch = a.get("channels", "?")
            labels.append(f"{i}: {lang} {title} ({ch}ch)".strip())
        self.a_box["values"] = labels
        if labels:
            self.a_box.current(0)
        self.recalc()
        self.stat.config(text="Pronto.", foreground="#080")

    def recalc(self):
        if not self.src_w or not self.src_h:
            return
        h = int(self.h_var.get())
        w = even16(h * self.src_w / self.src_h)     # keep aspect -> compute width, NO crop
        self.out_w, self.out_h = w, h
        self.dim_lbl.config(text=f"→ {w}×{h}  (AR {self.src_w/self.src_h:.2f}:1 preservato)")
        # size estimate
        kbps = int(self.br_var.get()) + 128
        mb = kbps * self.dur / 8 / 1024
        warn = ""
        if w * h > 74000:
            warn = "  ⚠ grande: rischio rallentatore sul device"
        self.size_lbl.config(text=f"~{mb:.0f} MB{warn}")

    # ---- conversion ----------------------------------------------------------
    def start(self):
        src = self.in_var.get(); dst = self.out_var.get()
        if not src or not os.path.exists(src):
            messagebox.showwarning("Input", "Scegli un file valido."); return
        if not dst:
            messagebox.showwarning("Output", "Scegli il file di uscita."); return
        self.recalc()
        amap = "0:a:0"
        if self.audios and self.a_box.current() >= 0:
            amap = f"0:{self.audios[self.a_box.current()]['index']}"
        cmd = [FFMPEG, "-hide_banner", "-y", "-i", src,
               "-map", "0:v:0", "-map", amap,
               "-c:v", "mpeg1video", "-s", f"{self.out_w}x{self.out_h}", "-r", "24", "-bf", "0",
               "-b:v", f"{int(self.br_var.get())}k",
               "-c:a", "mp2", "-b:a", "128k", "-ar", "48000", "-ac", "2",
               "-f", "mpeg", "-progress", "pipe:1", "-nostats", dst]
        self.logln("$ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        self.go.config(state="disabled"); self.cancel.config(state="normal")
        self.pb["value"] = 0
        threading.Thread(target=self._run, args=(cmd,), daemon=True).start()

    def _run(self, cmd):
        try:
            self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, creationflags=CREATE_NO_WINDOW, bufsize=1)
        except Exception as e:
            self.after(0, lambda: self._done(False, str(e))); return
        total_us = self.dur * 1_000_000
        for line in self.proc.stdout:
            line = line.strip()
            m = re.match(r"out_time_us=(\d+)", line)
            if m and total_us:
                frac = min(1.0, int(m.group(1)) / total_us)
                self.after(0, lambda f=frac: self._progress(f))
            elif line.startswith("progress=") and line.endswith("end"):
                self.after(0, lambda: self._progress(1.0))
        rc = self.proc.wait()
        ok = rc == 0 and os.path.exists(cmd[-1])
        self.after(0, lambda: self._done(ok, f"exit {rc}"))

    def _progress(self, frac):
        self.pb["value"] = frac * 1000
        self.stat.config(text=f"Conversione… {frac*100:.0f}%", foreground="#555")

    def _done(self, ok, msg):
        self.go.config(state="normal"); self.cancel.config(state="disabled")
        self.proc = None
        if ok:
            self.pb["value"] = 1000
            sz = os.path.getsize(self.out_var.get()) / 1024 / 1024
            self.stat.config(text=f"FATTO ✓  ({sz:.0f} MB) — copia il .mpg in /Video sulla SD",
                             foreground="#080")
            self.logln(f"OK: {self.out_var.get()}  ({sz:.1f} MB)")
        else:
            self.stat.config(text=f"Errore ({msg})", foreground="#c00")
            self.logln(f"FALLITO: {msg}")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            self.stat.config(text="Annullato.", foreground="#c00")


if __name__ == "__main__":
    App().mainloop()
