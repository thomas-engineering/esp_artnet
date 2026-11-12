"""
Art-Net test client for Showtec LED Par 64 Short Pro RGB (order 42427).

Sends Art-Net ArtDMX frames to the ESP32 Art-Net→DMX node.

Usage:
    python gui.py

Requires Python 3.10+ with tkinter.
On Ubuntu/Debian, install tkinter with: sudo apt install python3-tk
"""

import colorsys
import math
import tkinter as tk
from tkinter import ttk
import threading
import time

from artnet import ArtNetSender

# ── Lamp channel descriptors (from 42427 manual) ─────────────────────────────

# (start_value, name) for Ch6 built-in programs
_CH6_PROGRAMS = [
    (  0, "No function"),
    ( 32, "Slow fade in / out"),
    ( 64, "Full beam → slow fade"),
    ( 96, "Slow fade cycle"),
    (128, "Color flow"),
    (160, "3-color jump"),
    (192, "7-color jump"),
    (224, "Sound controlled"),
]

_CH6_BTN_LABELS = [
    "Off", "Fade", "Beam/Fade", "Fade cycle",
    "Color flow", "3-Jump", "7-Jump", "Sound",
]


def _ch4_desc(v: int) -> str:
    return "no function" if v <= 7 else "macro color"


def _ch6_desc(v: int) -> str:
    for start, name in reversed(_CH6_PROGRAMS):
        if v >= start:
            return name
    return ""


def _ch5_title(ch6: int) -> str:
    if ch6 <= 31:  return "Ch 5 — Strobe"
    if ch6 <= 223: return "Ch 5 — Speed"
    return "Ch 5 — Sensitivity"


def _ch5_desc(v: int, ch6: int) -> str:
    if ch6 <= 31:
        return "off" if v <= 15 else ("slow strobe" if v < 128 else "fast strobe")
    if ch6 <= 223:
        return "slow" if v < 85 else ("medium" if v < 170 else "fast")
    return "low" if v < 85 else ("medium" if v < 170 else "high sensitivity")


# ── Light show ────────────────────────────────────────────────────────────────
#
# LEDs are driven by PWM, so light output is *linear* in the DMX value, while
# human brightness perception is strongly non-linear (roughly output^(1/2.2)).
# A fade that is linear in DMX therefore looks like it jumps up quickly and
# then barely changes. The show is therefore computed in perceptual space
# (where steps look even) and converted to DMX values with a gamma curve.

_GAMMA        = 2.2
_SHOW_HZ      = 40
_SCENE_LEN    = 12.0   # seconds per scene
_SCENE_FADE   = 0.8    # seconds of fade in / out at scene boundaries


def perceptual_to_dmx(v: float) -> int:
    """Map a perceptual brightness 0..1 to a linear-light DMX value 0..255."""
    v = min(1.0, max(0.0, v))
    return round(255.0 * v ** _GAMMA)


def _smooth(x: float) -> float:
    """Smoothstep 0..1."""
    x = min(1.0, max(0.0, x))
    return x * x * (3.0 - 2.0 * x)


def _scene_rainbow(t: float):
    """Continuous rainbow sweep at full perceptual brightness."""
    return (t * 0.12) % 1.0, 1.0, 1.0


def _scene_breathe(t: float):
    """Slow breathing in a drifting hue (sine in perceptual space)."""
    v = 0.08 + 0.92 * (0.5 - 0.5 * math.cos(2 * math.pi * t / 4.0))
    return 0.55 + 0.25 * math.sin(t * 0.3), 1.0, v


def _scene_ember(t: float):
    """Warm flickering fire from layered sines."""
    f = (math.sin(t * 7.1) + math.sin(t * 11.3 + 1.7)
         + math.sin(t * 17.9 + 4.2)) / 3.0          # -1..1
    return 0.02 + 0.04 * (f + 1.0) / 2.0, 1.0, 0.55 + 0.35 * f


def _scene_heartbeat(t: float):
    """Double-pulse heartbeat, alternating magenta and cyan each beat."""
    beat = 1.2
    n, ph = divmod(t, beat)
    ph /= beat
    v = max(math.exp(-((ph - 0.10) / 0.05) ** 2),
            0.7 * math.exp(-((ph - 0.30) / 0.06) ** 2))
    return (0.85 if int(n) % 2 == 0 else 0.5), 1.0, 0.04 + 0.96 * v


def _scene_aurora(t: float):
    """Green-to-violet wandering hue with a gentle brightness swell."""
    hue = 0.40 + 0.25 * (0.5 + 0.5 * math.sin(t * 0.45))
    v = 0.45 + 0.55 * (0.5 + 0.5 * math.sin(t * 0.9 + 1.0))
    return hue, 0.85, v


_SCENES = (_scene_rainbow, _scene_breathe, _scene_ember,
           _scene_heartbeat, _scene_aurora)


def light_show_rgb(t: float) -> tuple[int, int, int]:
    """DMX (R, G, B) for time t seconds since the show started."""
    idx, local = divmod(t, _SCENE_LEN)
    scene = _SCENES[int(idx) % len(_SCENES)]
    h, s, v = scene(local)
    env = min(_smooth(local / _SCENE_FADE),
              _smooth((_SCENE_LEN - local) / _SCENE_FADE))
    r, g, b = colorsys.hsv_to_rgb(h % 1.0, s, v * env)
    return perceptual_to_dmx(r), perceptual_to_dmx(g), perceptual_to_dmx(b)


# ── Police car ────────────────────────────────────────────────────────────────
#
# US-style emergency light bar: red and blue take turns with short, hard
# on/off flashes (no fading - a strobe is on or off), then both colors
# alternate quickly. The LEDs are simply full on or off here, so the gamma
# curve above does not matter.

_POLICE_HZ     = 100    # update rate; the Art-Net sender runs at 44 Hz
_POLICE_FLASH  = 0.08   # one flash: 80 ms on, 80 ms off
_POLICE_PERIOD = 1.8    # s: red triple, blue triple, fast alternation


def police_rgb(t: float) -> tuple[int, int, int]:
    """DMX (R, G, B) of the police light bar at time t seconds."""
    ph = t % _POLICE_PERIOD
    f = _POLICE_FLASH
    if ph < 0.5:                       # red triple flash
        on = int(ph / f) % 2 == 0 and ph < 6 * f
        return (255, 0, 0) if on else (0, 0, 0)
    if 0.6 <= ph < 1.1:                # blue triple flash
        q = ph - 0.6
        on = int(q / f) % 2 == 0 and q < 6 * f
        return (0, 0, 255) if on else (0, 0, 0)
    if ph >= 1.2:                      # fast red / blue alternation
        return (255, 0, 0) if int((ph - 1.2) / 0.1) % 2 == 0 else (0, 0, 255)
    return (0, 0, 0)


# ── Application ───────────────────────────────────────────────────────────────

class App(tk.Tk):
    _SEND_HZ = 44

    def __init__(self):
        super().__init__()
        self.title("Art-Net Test  –  Showtec LED Par 64 Short Pro")
        self.resizable(False, False)

        # Connection
        self._v_host     = tk.StringVar(value="dmx-artnet.local")
        self._v_universe = tk.IntVar(value=0)
        self._v_start_ch = tk.IntVar(value=1)
        self._v_mode     = tk.StringVar(value="6")

        # DMX channel values: index i = Ch(i+1)
        self._ch = [tk.IntVar(value=0) for _ in range(6)]

        # Sender state
        self._sender: ArtNetSender | None = None
        self._blackout   = False
        self._running    = True
        self._pkt_ok     = 0
        self._pkt_fail   = 0
        # Running automatic effect (light show / police car), or None
        self._fx: tuple | None = None     # (rgb_fn, button, idle_text, hz)
        self._fx_t0      = 0.0
        self._fx_saved: list[int] = []

        self._build_ui()
        self._add_traces()
        self._start_sender()
        self.after(1000, self._tick_status)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ── UI construction ───────────────────────────────────────────────────────

    def _build_ui(self) -> None:
        P = dict(padx=8, pady=4)

        f = ttk.LabelFrame(self, text="Connection")
        f.grid(row=0, column=0, sticky="ew", **P)
        self._build_connection(f)

        f = ttk.LabelFrame(self, text="Color  (Ch 1–3)")
        f.grid(row=1, column=0, sticky="ew", **P)
        self._build_color(f)

        self._frame_6ch = ttk.LabelFrame(self, text="6-Channel Settings")
        self._frame_6ch.grid(row=2, column=0, sticky="ew", **P)
        self._build_6ch(self._frame_6ch)

        self._v_status = tk.StringVar(value="Connecting…")
        ttk.Label(self, textvariable=self._v_status, relief="sunken",
                  anchor="w").grid(row=3, column=0, sticky="ew",
                                   padx=8, pady=(0, 6))

        self._on_mode_change()  # set initial 6ch visibility

    def _build_connection(self, p: ttk.LabelFrame) -> None:
        ttk.Label(p, text="Target:").grid(
            row=0, column=0, sticky="e", padx=(8, 2), pady=4)
        ttk.Entry(p, textvariable=self._v_host, width=26).grid(
            row=0, column=1, sticky="w", padx=2)
        ttk.Label(p, text="Universe:").grid(
            row=0, column=2, sticky="e", padx=(12, 2))
        ttk.Spinbox(p, textvariable=self._v_universe, from_=0, to=32767,
                    width=7).grid(row=0, column=3, sticky="w", padx=2)

        ttk.Label(p, text="DMX address:").grid(
            row=1, column=0, sticky="e", padx=(8, 2), pady=4)
        ttk.Spinbox(p, textvariable=self._v_start_ch, from_=1, to=507,
                    width=7).grid(row=1, column=1, sticky="w", padx=2)
        ttk.Label(p, text="Mode:").grid(
            row=1, column=2, sticky="e", padx=(12, 2))
        mf = ttk.Frame(p)
        mf.grid(row=1, column=3, sticky="w", padx=2)
        for val, label in (("3", "3 ch"), ("6", "6 ch")):
            ttk.Radiobutton(mf, text=label, variable=self._v_mode,
                            value=val, command=self._on_mode_change).pack(
                side="left", padx=(0, 8))

        self._btn_blackout = ttk.Button(
            p, text="BLACKOUT", command=self._toggle_blackout, width=12)
        self._btn_blackout.grid(row=0, column=4, rowspan=2,
                                padx=(14, 4), pady=4)

        self._btn_show = ttk.Button(
            p, text="\u2728 Light show", width=14,
            command=lambda: self._toggle_effect(
                light_show_rgb, self._btn_show, "\u2728 Light show", _SHOW_HZ))
        self._btn_show.grid(row=0, column=5, padx=(4, 8), pady=(4, 1))

        self._btn_police = ttk.Button(
            p, text="Police car", width=14,
            command=lambda: self._toggle_effect(
                police_rgb, self._btn_police, "Police car", _POLICE_HZ))
        self._btn_police.grid(row=1, column=5, padx=(4, 8), pady=(1, 4))

    def _scale_row(self, parent, row: int, label: str,
                   var: tk.IntVar) -> None:
        """Labeled slider row: label | scale(0–255) | value."""
        ttk.Label(parent, text=label, width=15, anchor="e").grid(
            row=row, column=0, sticky="e", padx=(8, 2), pady=2)
        s = ttk.Scale(parent, from_=0, to=255, variable=var,
                      orient="horizontal", length=270)
        s.grid(row=row, column=1, padx=4, pady=2)
        # Snap float to integer
        s.configure(command=lambda v, _v=var: _v.set(int(float(v))))
        ttk.Label(parent, textvariable=var, width=4, anchor="w").grid(
            row=row, column=2, padx=(2, 4))

    def _build_color(self, p: ttk.LabelFrame) -> None:
        self._scale_row(p, 0, "Red",   self._ch[0])
        self._scale_row(p, 1, "Green", self._ch[1])
        self._scale_row(p, 2, "Blue",  self._ch[2])

        self._canvas = tk.Canvas(p, width=68, height=68, bd=2,
                                 relief="groove", bg="#000000")
        self._canvas.grid(row=0, column=3, rowspan=3, padx=10, pady=4)
        self._rect = self._canvas.create_rectangle(
            0, 0, 68, 68, fill="#000000", outline="")

    def _build_6ch(self, p: ttk.LabelFrame) -> None:
        # Ch 4 — Macro colors
        self._scale_row(p, 0, "Ch 4 — Macro", self._ch[3])
        self._v_ch4_desc = tk.StringVar(value=_ch4_desc(0))
        ttk.Label(p, textvariable=self._v_ch4_desc, foreground="#666666",
                  anchor="w").grid(row=0, column=3, sticky="w", padx=4)

        # Ch 6 — Built-in programs
        self._scale_row(p, 1, "Ch 6 — Program", self._ch[5])
        self._v_ch6_desc = tk.StringVar(value=_ch6_desc(0))
        ttk.Label(p, textvariable=self._v_ch6_desc, foreground="#666666",
                  anchor="w").grid(row=1, column=3, sticky="w", padx=4)

        # Ch6 preset buttons
        bf = ttk.Frame(p)
        bf.grid(row=2, column=0, columnspan=5, padx=8, pady=(2, 6),
                sticky="w")
        for (val, _), label in zip(_CH6_PROGRAMS, _CH6_BTN_LABELS):
            ttk.Button(bf, text=label, width=10,
                       command=lambda v=val: self._ch[5].set(v)).pack(
                side="left", padx=2)

        # Ch 5 — Strobe / Speed / Sensitivity (title changes with Ch6)
        self._v_ch5_title = tk.StringVar(value="Ch 5 — Strobe")
        ttk.Label(p, textvariable=self._v_ch5_title, width=15,
                  anchor="e").grid(row=3, column=0, sticky="e",
                                   padx=(8, 2), pady=(6, 2))
        s5 = ttk.Scale(p, from_=0, to=255, variable=self._ch[4],
                       orient="horizontal", length=270)
        s5.grid(row=3, column=1, padx=4, pady=(6, 2))
        s5.configure(command=lambda v: self._ch[4].set(int(float(v))))
        ttk.Label(p, textvariable=self._ch[4], width=4, anchor="w").grid(
            row=3, column=2, padx=(2, 4))
        self._v_ch5_desc = tk.StringVar(value=_ch5_desc(0, 0))
        ttk.Label(p, textvariable=self._v_ch5_desc, foreground="#666666",
                  anchor="w").grid(row=3, column=3, sticky="w", padx=4)

    def _add_traces(self) -> None:
        for i in range(3):
            self._ch[i].trace_add("write", lambda *_: self._update_preview())
        self._ch[3].trace_add("write",
            lambda *_: self._v_ch4_desc.set(_ch4_desc(self._ch[3].get())))
        self._ch[4].trace_add("write",
            lambda *_: self._v_ch5_desc.set(
                _ch5_desc(self._ch[4].get(), self._ch[5].get())))
        self._ch[5].trace_add("write", lambda *_: self._on_ch6_change())

        # Populate all labels with initial values
        for v in self._ch:
            v.set(v.get())

    # ── Event handlers ────────────────────────────────────────────────────────

    def _update_preview(self) -> None:
        r, g, b = self._ch[0].get(), self._ch[1].get(), self._ch[2].get()
        self._canvas.itemconfig(self._rect, fill=f"#{r:02x}{g:02x}{b:02x}")

    def _on_ch6_change(self) -> None:
        v = self._ch[5].get()
        self._v_ch6_desc.set(_ch6_desc(v))
        self._v_ch5_title.set(_ch5_title(v))
        self._v_ch5_desc.set(_ch5_desc(self._ch[4].get(), v))

    def _on_mode_change(self) -> None:
        if self._v_mode.get() == "6":
            self._frame_6ch.grid()
        else:
            self._frame_6ch.grid_remove()

    def _toggle_blackout(self) -> None:
        self._blackout = not self._blackout
        self._btn_blackout.configure(
            text="● BLACKOUT ●" if self._blackout else "BLACKOUT")

    # ── Automatic effects (light show / police car) ───────────────────────────

    def _toggle_effect(self, fn, button, idle_text: str, hz: int) -> None:
        """Start effect fn on the RGB channels; stop it if it is running."""
        running = self._fx
        if running:
            self._stop_effect()
            if running[1] is button:
                return              # same button again: just stop
        # Remember the current values and switch off the lamp's own
        # macro / strobe / program channels so only RGB is in effect.
        self._fx_saved = [v.get() for v in self._ch]
        for i in (3, 4, 5):
            self._ch[i].set(0)
        self._fx = (fn, button, idle_text, hz)
        self._fx_t0 = time.monotonic()
        button.configure(text="\u25a0 Stop")
        self._effect_step()

    def _stop_effect(self) -> None:
        if not self._fx:
            return
        _, button, idle_text, _ = self._fx
        self._fx = None
        button.configure(text=idle_text)
        for v, val in zip(self._ch, self._fx_saved):
            v.set(val)

    def _effect_step(self) -> None:
        if not self._fx:
            return
        fn, _, _, hz = self._fx
        r, g, b = fn(time.monotonic() - self._fx_t0)
        self._ch[0].set(r)
        self._ch[1].set(g)
        self._ch[2].set(b)
        self.after(int(1000 / hz), self._effect_step)

    # ── DMX frame ────────────────────────────────────────────────────────────

    def _build_frame(self) -> bytearray:
        frame = bytearray(512)
        if self._blackout:
            return frame
        offset = max(0, self._v_start_ch.get() - 1)
        n = 3 if self._v_mode.get() == "3" else 6
        for i in range(n):
            idx = offset + i
            if idx < 512:
                frame[idx] = self._ch[i].get()
        return frame

    # ── Sender thread ─────────────────────────────────────────────────────────

    def _start_sender(self) -> None:
        self._sender = ArtNetSender(self._v_host.get())
        threading.Thread(target=self._send_loop, daemon=True).start()

    def _send_loop(self) -> None:
        interval = 1.0 / self._SEND_HZ
        while self._running:
            t0 = time.monotonic()

            host = self._v_host.get()
            if self._sender.host != host:
                self._sender = ArtNetSender(host)

            ok = self._sender.send(self._v_universe.get(), self._build_frame())
            if ok:
                self._pkt_ok += 1
            else:
                self._pkt_fail += 1

            gap = interval - (time.monotonic() - t0)
            if gap > 0:
                time.sleep(gap)

    def _tick_status(self) -> None:
        ok   = self._pkt_ok;   self._pkt_ok   = 0
        fail = self._pkt_fail; self._pkt_fail = 0
        ip   = self._sender.resolved_ip if self._sender else None

        if ip:
            s = f"→  {self._v_host.get()}  ({ip})  ·  {ok} pkt/s"
            if fail:
                s += f"  ·  {fail} errors"
        else:
            s = f"✗  Cannot resolve '{self._v_host.get()}'"

        self._v_status.set(s)
        self.after(1000, self._tick_status)

    def _on_close(self) -> None:
        self._running = False
        if self._sender:
            self._sender.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
