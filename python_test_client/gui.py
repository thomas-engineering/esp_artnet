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
from tkinter import ttk, messagebox
import threading
import time

import queue
import artnet
from artnet import ArtNetSender, NodeLink, query_node, scan_nodes

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


# ── Scan dialog ───────────────────────────────────────────────────────────────

class ScanDialog(tk.Toplevel):
    """Broadcasts ArtPoll, lists the devices that answer and lets the user
    pick one. `on_select(node_dict)` is called with the chosen device."""

    _SCAN_S = 3.0

    def __init__(self, parent: tk.Tk, on_select) -> None:
        super().__init__(parent)
        self.title("Scan for Art-Net devices")
        self.transient(parent)
        self.resizable(False, False)
        self._on_select = on_select
        self._nodes: dict[str, dict] = {}     # tree item id -> node
        self._found: list[dict] = []          # filled by the scan thread
        self._stop = threading.Event()
        self._scanning = False

        cols = ("name", "ip", "universe", "type")
        self._tree = ttk.Treeview(self, columns=cols, show="headings",
                                  height=8, selectmode="browse")
        for col, text, width in (("name", "Name", 170), ("ip", "IP address", 120),
                                 ("universe", "Art-Net Port-Adr.", 110),
                                 ("type", "Type", 90)):
            self._tree.heading(col, text=text)
            self._tree.column(col, width=width, anchor="w")
        self._tree.grid(row=0, column=0, columnspan=3, padx=8, pady=(8, 4))
        self._tree.bind("<<TreeviewSelect>>", self._on_tree_select)
        self._tree.bind("<Double-1>", lambda _e: self._use())

        self._v_status = tk.StringVar()
        ttk.Label(self, textvariable=self._v_status, foreground="#666666",
                  anchor="w").grid(row=1, column=0, columnspan=3,
                                   sticky="ew", padx=8)

        bf = ttk.Frame(self)
        bf.grid(row=2, column=0, columnspan=3, sticky="e", padx=8, pady=8)
        self._btn_rescan = ttk.Button(bf, text="Rescan", command=self._scan)
        self._btn_rescan.pack(side="left", padx=2)
        self._btn_use = ttk.Button(bf, text="Use selected", command=self._use,
                                   state="disabled")
        self._btn_use.pack(side="left", padx=2)
        ttk.Button(bf, text="Cancel", command=self._close).pack(
            side="left", padx=2)

        self.protocol("WM_DELETE_WINDOW", self._close)
        self.bind("<Escape>", lambda _e: self._close())
        self._scan()

    def _scan(self) -> None:
        if self._scanning:
            return
        self._scanning = True
        self._stop.clear()
        self._found = []
        self._nodes.clear()
        self._tree.delete(*self._tree.get_children())
        self._btn_use.configure(state="disabled")
        self._btn_rescan.configure(state="disabled")
        self._v_status.set("Scanning…")
        found = self._found

        def work() -> None:
            try:
                scan_nodes(self._SCAN_S, on_found=found.append, stop=self._stop)
            finally:
                found.append(None)            # marks the end of the scan
        threading.Thread(target=work, daemon=True).start()
        self._poll(0)

    def _poll(self, shown: int) -> None:
        """Move devices found by the scan thread into the list."""
        items = self._found
        while shown < len(items):
            node = items[shown]
            shown += 1
            if node is None:
                self._scanning = False
                self._btn_rescan.configure(state="normal")
                n = len(self._nodes)
                self._v_status.set(
                    f"{n} device{'s' if n != 1 else ''} found" if n
                    else "No devices found")
                return
            # port-address as net:sub-net:universe
            uni = ("–" if node["universe"] is None else
                   f"{node['net']}:{node['sub']}:{node['uni']}")
            iid = self._tree.insert("", "end", values=(
                node["name"] or node["long_name"], node["ip"], uni,
                node["style"]))
            self._nodes[iid] = node
        self.after(100, self._poll, shown)

    def _on_tree_select(self, _event=None) -> None:
        self._btn_use.configure(
            state="normal" if self._tree.selection() else "disabled")

    def _use(self) -> None:
        sel = self._tree.selection()
        if not sel:
            return
        self._on_select(self._nodes[sel[0]])
        self._close()

    def _close(self) -> None:
        self._stop.set()
        self.destroy()


# ── Node control dialog (ArtAddress / unsolicited ArtPollReply) ───────────────

class NodeDialog(tk.Toplevel):
    """Test bench for the node's remote-programming features: ArtAddress
    (port-address, names, LED commands, clear output) and ArtPollReply
    (answers to a poll and unsolicited updates)."""

    def __init__(self, parent: tk.Tk, node: dict, on_universe,
                 on_target=None) -> None:
        super().__init__(parent)
        self.title("Node control  –  ArtAddress test")
        self.transient(parent)
        self.resizable(False, False)
        self._ip = node["ip"]
        self._on_universe = on_universe
        self._on_target = on_target
        self._rx: queue.Queue = queue.Queue()
        self._ip_rx: queue.Queue = queue.Queue()
        self._link = NodeLink(lambda n, a: self._rx.put((n, a)),
                              self._ip_rx.put)

        self._v_info = tk.StringVar()
        self._v_net  = tk.IntVar(value=node.get("net") or 0)
        self._v_sub  = tk.IntVar(value=node.get("sub") or 0)
        self._v_uni  = tk.IntVar(value=node.get("uni") or 0)
        self._v_sname = tk.StringVar(value=node.get("name", ""))
        self._v_lname = tk.StringVar(value=node.get("long_name", ""))
        self._v_follow = tk.BooleanVar(value=True)
        self._v_ipmode = tk.StringVar(value="dhcp" if node.get("dhcp") else "static")
        self._v_ip   = tk.StringVar(value=node["ip"])
        self._v_mask = tk.StringVar(value="255.255.255.0")
        self._v_gw   = tk.StringVar(value="")
        self._v_follow_ip = tk.BooleanVar(value=True)

        P = dict(padx=8, pady=4)
        ttk.Label(self, textvariable=self._v_info, anchor="w",
                  relief="sunken").grid(row=0, column=0, sticky="ew", **P)

        # Port-address
        f = ttk.LabelFrame(self, text="Port-address  (ArtAddress)")
        f.grid(row=1, column=0, sticky="ew", **P)
        for col, (text, var, hi) in enumerate((
                ("Net", self._v_net, 127), ("Sub-net", self._v_sub, 15),
                ("Universe", self._v_uni, 15))):
            ttk.Label(f, text=text).grid(row=0, column=2 * col, padx=(8, 2))
            ttk.Spinbox(f, textvariable=var, from_=0, to=hi, width=5).grid(
                row=0, column=2 * col + 1, padx=(0, 6), pady=4)
        ttk.Button(f, text="Apply address", command=self._apply_address).grid(
            row=0, column=6, padx=8)
        ttk.Checkbutton(f, text="Also set the GUI's universe to the new "
                        "address", variable=self._v_follow).grid(
            row=1, column=0, columnspan=7, sticky="w", padx=8, pady=(0, 4))

        # Names
        f = ttk.LabelFrame(self, text="Names  (ArtAddress)")
        f.grid(row=2, column=0, sticky="ew", **P)
        ttk.Label(f, text="Short (17):").grid(row=0, column=0, sticky="e", padx=4)
        ttk.Entry(f, textvariable=self._v_sname, width=20).grid(
            row=0, column=1, sticky="w", pady=2)
        ttk.Label(f, text="Long (63):").grid(row=1, column=0, sticky="e", padx=4)
        ttk.Entry(f, textvariable=self._v_lname, width=48).grid(
            row=1, column=1, sticky="w", pady=2)
        ttk.Button(f, text="Apply names", command=self._apply_names).grid(
            row=0, column=2, rowspan=2, padx=8)

        # Commands: a grid of 5 columns keeps the dialog narrow
        f = ttk.LabelFrame(self, text="Commands  (ArtAddress)")
        f.grid(row=3, column=0, sticky="ew", **P)
        commands = (("Locate (blink)", artnet.AC_LED_LOCATE),
                    ("Mute LEDs", artnet.AC_LED_MUTE),
                    ("LEDs normal", artnet.AC_LED_NORMAL),
                    ("Clear output", artnet.AC_CLEAR_OP0),
                    ("Cancel merge", artnet.AC_CANCEL_MERGE),
                    ("Failsafe: hold", artnet.AC_FAIL_HOLD),
                    ("Failsafe: zero", artnet.AC_FAIL_ZERO),
                    ("Failsafe: full", artnet.AC_FAIL_FULL),
                    ("Merge HTP", artnet.AC_MERGE_HTP0),
                    ("Merge LTP", artnet.AC_MERGE_LTP0))
        for i, (text, cmd) in enumerate(commands):
            ttk.Button(f, text=text, width=13,
                       command=lambda c=cmd, t=text: self._command(c, t)).grid(
                row=i // 5, column=i % 5, padx=3, pady=3)

        # IP configuration
        f = ttk.LabelFrame(self, text="IP configuration  (ArtIpProg)")
        f.grid(row=4, column=0, sticky="ew", **P)
        for col, (text, val) in enumerate((("DHCP", "dhcp"),
                                           ("Static IP", "static"))):
            ttk.Radiobutton(f, text=text, variable=self._v_ipmode, value=val,
                            command=self._ip_mode_changed).grid(
                row=0, column=col, sticky="w", padx=8)
        ttk.Button(f, text="Read", width=8, command=self._ip_read).grid(
            row=0, column=3, sticky="e", padx=8, pady=2)
        self._ip_entries = []
        for row, (text, var) in enumerate((("IP address:", self._v_ip),
                                           ("Subnet mask:", self._v_mask),
                                           ("Gateway:", self._v_gw)), start=1):
            ttk.Label(f, text=text).grid(row=row, column=0, sticky="e", padx=4)
            e = ttk.Entry(f, textvariable=var, width=18)
            e.grid(row=row, column=1, columnspan=2, sticky="w", pady=1)
            self._ip_entries.append(e)
        ttk.Button(f, text="Apply (node restarts)", command=self._ip_apply).grid(
            row=1, column=3, rowspan=2, padx=8)
        ttk.Checkbutton(f, text="Switch the GUI target to the new IP",
                        variable=self._v_follow_ip).grid(
            row=4, column=0, columnspan=4, sticky="w", padx=8, pady=(0, 4))
        self._ip_mode_changed()

        # Discovery
        f = ttk.LabelFrame(self, text="Discovery  (ArtPoll / ArtPollReply)")
        f.grid(row=5, column=0, sticky="ew", **P)
        ttk.Button(f, text="Poll node", width=14,
                   command=self._poll).pack(side="left", padx=4, pady=4)
        ttk.Button(f, text="Request unsolicited replies", width=26,
                   command=self._request_unsolicited).pack(
            side="left", padx=4, pady=4)

        # Log
        f = ttk.LabelFrame(self, text="Log")
        f.grid(row=6, column=0, sticky="ew", **P)
        self._log = tk.Text(f, width=78, height=10, state="disabled",
                            wrap="word")
        self._log.pack(padx=4, pady=4)

        ttk.Button(self, text="Close", command=self._close).grid(
            row=7, column=0, sticky="e", padx=8, pady=(0, 8))

        self._show_info(node)
        if not self._link.bound:
            self._say("note: UDP port 6454 is in use by another program, "
                      "unsolicited broadcasts cannot be received")
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.bind("<Escape>", lambda _e: self._close())
        self.after(100, self._drain)

    # ── helpers ──────────────────────────────────────────────────────────────

    def _say(self, text: str) -> None:
        self._log.configure(state="normal")
        self._log.insert("end", time.strftime("%H:%M:%S  ") + text + "\n")
        self._log.see("end")
        self._log.configure(state="disabled")

    def _show_info(self, n: dict) -> None:
        uni = "–" if n["universe"] is None else (
            f"{n['net']}:{n['sub']}:{n['uni']}  ({n['universe']})")
        self._v_info.set(
            f" {n['ip']}   {n['name']!r}   port-address {uni}   "
            f"LEDs: {n['indicator']}   {'DHCP' if n['dhcp'] else 'static IP'}   "
            f"failsafe: {n['failsafe']}   merge: {n['merge']}"
            f"{' (merging)' if n['merging'] else ''}")

    def _drain(self) -> None:
        """Show replies received by the link thread."""
        try:
            while True:
                node, addr = self._rx.get_nowait()
                if node["ip"] != self._ip:
                    continue
                self._show_info(node)
                self._say(f"\u2190 ArtPollReply from {addr[0]}:{addr[1]}  "
                          f"name={node['name']!r}  long={node['long_name']!r}  "
                          f"port-address={node['universe']}  "
                          f"LEDs={node['indicator']}  failsafe={node['failsafe']}  "
                          f"merge={node['merge']}{'*' if node['merging'] else ''}  "
                          f"{node['report']}")
        except queue.Empty:
            pass
        try:
            while True:
                r = self._ip_rx.get_nowait()
                self._say(f"\u2190 ArtIpProgReply from {r['from']}:  "
                          f"{'DHCP' if r['dhcp'] else 'static'}  ip={r['ip']}  "
                          f"mask={r['mask']}  gateway={r['gateway']}")
                self._fill_ip(r)
        except queue.Empty:
            pass
        if self.winfo_exists():
            self.after(100, self._drain)

    def _send(self, packet: bytes, what: str) -> None:
        ok = self._link.send(self._ip, packet)
        self._say(f"\u2192 {what}" + ("" if ok else "   (send failed)"))

    # ── actions ──────────────────────────────────────────────────────────────

    def _apply_address(self) -> None:
        net, sub, uni = (self._v_net.get(), self._v_sub.get(),
                         self._v_uni.get())
        if not (0 <= net <= 127 and 0 <= sub <= 15 and 0 <= uni <= 15):
            self._say("invalid address (net 0-127, sub-net 0-15, universe 0-15)")
            return
        pa = (net << 8) | (sub << 4) | uni
        self._send(artnet.build_art_address(net=net, sub=sub, universe=uni),
                   f"ArtAddress: port-address {net}:{sub}:{uni} ({pa})")
        if self._v_follow.get():
            self._on_universe(pa)

    def _apply_names(self) -> None:
        self._send(artnet.build_art_address(
            short_name=self._v_sname.get(), long_name=self._v_lname.get()),
            "ArtAddress: names")

    def _command(self, command: int, label: str) -> None:
        self._send(artnet.build_art_address(command=command),
                   f"ArtAddress: command '{label}'")

    def _poll(self) -> None:
        self._send(artnet.build_poll(), "ArtPoll")

    def _request_unsolicited(self) -> None:
        self._send(artnet.build_poll(artnet.POLL_TALK_ON_CHANGE),
                   "ArtPoll with TalkToMe 'reply on change' - now change "
                   "something above and watch for broadcasts")

    # ── IP configuration (ArtIpProg) ─────────────────────────────────────────

    def _ip_mode_changed(self) -> None:
        state = "normal" if self._v_ipmode.get() == "static" else "disabled"
        for e in self._ip_entries:
            e.configure(state=state)

    def _fill_ip(self, r: dict) -> None:
        self._v_ipmode.set("dhcp" if r["dhcp"] else "static")
        self._v_ip.set(r["ip"])
        self._v_mask.set(r["mask"])
        self._v_gw.set(r["gateway"])
        self._ip_mode_changed()

    def _ip_read(self) -> None:
        self._send(artnet.build_ip_prog(program=False),
                   "ArtIpProg: query current IP settings")

    def _ip_apply(self) -> None:
        dhcp = self._v_ipmode.get() == "dhcp"
        try:
            if dhcp:
                pkt = artnet.build_ip_prog(dhcp=True)
                what = "enable DHCP"
            else:
                ip, mask, gw = (self._v_ip.get().strip(),
                                self._v_mask.get().strip(),
                                self._v_gw.get().strip())
                pkt = artnet.build_ip_prog(ip=ip, mask=mask, gateway=gw or None)
                what = f"static {ip} / {mask} / {gw or '-'}"
        except OSError:
            self._say("invalid IP address, mask or gateway")
            return
        if not messagebox.askokcancel(
                "Reconfigure node",
                f"Set the node to: {what}\n\nThe node restarts to apply this "
                "and may get a different address. If it becomes unreachable, "
                "hold the BOOT button for 3 s to reset its network settings.",
                parent=self):
            return
        self._send(pkt, f"ArtIpProg: {what}")
        if not dhcp and self._v_follow_ip.get() and self._on_target:
            self._on_target(self._v_ip.get().strip())

    def _close(self) -> None:
        self._link.close()
        self.destroy()


# ── Application ───────────────────────────────────────────────────────────────

class App(tk.Tk):
    _SEND_HZ = 44

    def __init__(self):
        super().__init__()
        self.title("Art-Net Test  –  Showtec LED Par 64 Short Pro")
        self.resizable(False, False)

        # Connection
        self._v_host     = tk.StringVar(value="dmx-artnet.local")
        self._v_universe = tk.IntVar(value=0)     # 15-bit port-address (used by the sender)
        self._v_net      = tk.IntVar(value=0)     # 0-127
        self._v_sub      = tk.IntVar(value=0)     # 0-15
        self._v_uni      = tk.IntVar(value=0)     # 0-15
        self._pa_syncing = False
        self._v_start_ch = tk.IntVar(value=1)
        self._v_mode     = tk.StringVar(value="6")

        # DMX channel values: index i = Ch(i+1)
        self._ch = [tk.IntVar(value=0) for _ in range(6)]

        # Values used by the background threads. Tk variables must only be
        # touched from the main thread, so they are mirrored here on change.
        self._tx_host     = self._v_host.get()
        self._tx_universe = 0
        self._tx_frame    = bytes(512)

        # Sender state
        self._sender: ArtNetSender | None = None
        self._blackout   = False
        self._running    = True
        self._pkt_ok     = 0
        self._pkt_fail   = 0
        # Latest ArtPollReply of the target node (None = no node answering)
        self._node_info: dict | None = None
        self._node_dialog: "NodeDialog | None" = None

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
        ttk.Entry(p, textvariable=self._v_host, width=22).grid(
            row=0, column=1, sticky="w", padx=2)
        ttk.Button(p, text="Scan", width=13,
                   command=self._open_scan).grid(row=0, column=2, padx=4)
        # Art-Net port-address = net (0-127) : sub-net (0-15) : universe (0-15)
        uf = ttk.Frame(p)
        uf.grid(row=0, column=3, columnspan=2, sticky="w", padx=(12, 2))
        vcmd = (self.register(self._valid_digits), "%P", "%W")
        for col, (text, var, hi) in enumerate((
                ("Net", self._v_net, 127), ("Sub-net", self._v_sub, 15),
                ("Universe", self._v_uni, 15))):
            ttk.Label(uf, text=text).grid(row=0, column=2 * col,
                                          padx=(0 if col == 0 else 8, 2))
            sb = ttk.Spinbox(uf, textvariable=var, from_=0, to=hi, width=4,
                             validate="key", validatecommand=vcmd)
            sb.grid(row=0, column=2 * col + 1)
            sb._max = hi
        for var in (self._v_net, self._v_sub, self._v_uni):
            var.trace_add("write", lambda *_: self._parts_to_port_address())
        self._v_universe.trace_add("write",
                                   lambda *_: self._port_address_to_parts())
        self._port_address_to_parts()

        ttk.Label(p, text="DMX address:").grid(
            row=1, column=0, sticky="e", padx=(8, 2), pady=4)
        ttk.Spinbox(p, textvariable=self._v_start_ch, from_=1, to=507,
                    width=7).grid(row=1, column=1, sticky="w", padx=2)
        # Enabled while a node answers ArtPoll on the target address
        self._btn_node = ttk.Button(p, text="Node control", width=13,
                                    command=self._open_node_dialog,
                                    state="disabled")
        self._btn_node.grid(row=1, column=2, padx=4)
        ttk.Label(p, text="Mode:").grid(
            row=1, column=3, sticky="e", padx=(12, 2))
        mf = ttk.Frame(p)
        mf.grid(row=1, column=4, sticky="w", padx=2)
        for val, label in (("3", "3 ch"), ("6", "6 ch")):
            ttk.Radiobutton(mf, text=label, variable=self._v_mode,
                            value=val, command=self._on_mode_change).pack(
                side="left", padx=(0, 8))

        self._btn_blackout = ttk.Button(
            p, text="BLACKOUT", command=self._toggle_blackout, width=12)
        self._btn_blackout.grid(row=0, column=5, rowspan=2,
                                padx=(14, 4), pady=4)

        self._btn_show = ttk.Button(
            p, text="\u2728 Light show", width=14,
            command=lambda: self._toggle_effect(
                light_show_rgb, self._btn_show, "\u2728 Light show", _SHOW_HZ))
        self._btn_show.grid(row=0, column=6, padx=(4, 8), pady=(4, 1))

        self._btn_police = ttk.Button(
            p, text="Police car", width=14,
            command=lambda: self._toggle_effect(
                police_rgb, self._btn_police, "Police car", _POLICE_HZ))
        self._btn_police.grid(row=1, column=6, padx=(4, 8), pady=(1, 4))

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

        # Keep the sender threads' copy of the state up to date
        for v in (self._v_host, self._v_universe, self._v_start_ch,
                  self._v_mode, *self._ch):
            v.trace_add("write", lambda *_: self._refresh_tx())

        # Populate all labels with initial values
        for v in self._ch:
            v.set(v.get())
        self._refresh_tx()

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

    def _valid_digits(self, proposed: str, widget: str) -> bool:
        """Spinbox validation: only digits, and not above the field's maximum."""
        if proposed == "":
            return True
        if not proposed.isdigit():
            return False
        return int(proposed) <= self.nametowidget(widget)._max

    def _parts_to_port_address(self) -> None:
        if self._pa_syncing:
            return
        try:
            pa = (self._v_net.get() << 8) | (self._v_sub.get() << 4) \
                 | self._v_uni.get()
        except tk.TclError:                       # a field is empty
            return
        self._pa_syncing = True
        self._v_universe.set(pa)
        self._pa_syncing = False

    def _port_address_to_parts(self) -> None:
        """The port-address was set from outside (scan, node dialog)."""
        if self._pa_syncing:
            return
        try:
            pa = min(max(self._v_universe.get(), 0), 32767)
        except tk.TclError:
            return
        self._pa_syncing = True
        self._v_net.set(pa >> 8)
        self._v_sub.set((pa >> 4) & 15)
        self._v_uni.set(pa & 15)
        self._pa_syncing = False

    def _open_scan(self) -> None:
        ScanDialog(self, self._apply_scan_result)

    def _apply_scan_result(self, node: dict) -> None:
        """Use the device picked in the scan dialog as target."""
        self._v_host.set(node["ip"])
        if node["universe"] is not None:
            self._v_universe.set(node["universe"])

    def _open_node_dialog(self) -> None:
        if self._node_dialog is not None and self._node_dialog.winfo_exists():
            self._node_dialog.lift()
            return
        if not self._node_info:
            return
        self._node_dialog = NodeDialog(
            self, self._node_info, self._on_node_universe,
            self._v_host.set)

    def _on_node_universe(self, port_address: int) -> None:
        """The node was re-addressed: keep sending to the new universe."""
        self._v_universe.set(port_address)

    def _toggle_blackout(self) -> None:
        self._blackout = not self._blackout
        self._btn_blackout.configure(
            text="● BLACKOUT ●" if self._blackout else "BLACKOUT")
        self._refresh_tx()

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

    def _build_frame(self) -> bytes:
        frame = bytearray(512)
        if self._blackout:
            return bytes(frame)
        offset = max(0, self._v_start_ch.get() - 1)
        n = 3 if self._v_mode.get() == "3" else 6
        for i in range(n):
            idx = offset + i
            if idx < 512:
                frame[idx] = min(max(self._ch[i].get(), 0), 255)
        return bytes(frame)

    def _refresh_tx(self) -> None:
        """Mirror the GUI state for the sender threads (main thread only)."""
        try:
            self._tx_host     = self._v_host.get()
            self._tx_universe = self._v_universe.get()
            self._tx_frame    = self._build_frame()
        except tk.TclError:
            pass                        # a field is empty while being edited

    # ── Sender thread ─────────────────────────────────────────────────────────

    def _start_sender(self) -> None:
        self._sender = ArtNetSender(self._v_host.get())
        threading.Thread(target=self._send_loop, daemon=True).start()
        threading.Thread(target=self._node_poll_loop, daemon=True).start()

    def _node_poll_loop(self) -> None:
        """Polls the target every 2 s; a reply means an Art-Net node is connected."""
        while self._running:
            self._node_info = query_node(self._tx_host)
            for _ in range(20):                  # 2 s, but react quickly to exit
                if not self._running:
                    return
                time.sleep(0.1)

    def _send_loop(self) -> None:
        interval = 1.0 / self._SEND_HZ
        while self._running:
            t0 = time.monotonic()

            host = self._tx_host
            if self._sender.host != host:
                self._sender.close()
                self._sender = ArtNetSender(host)

            ok = self._sender.send(self._tx_universe, self._tx_frame)
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
        self._btn_node.configure(
            state="normal" if self._node_info else "disabled")
        self.after(1000, self._tick_status)

    def _on_close(self) -> None:
        self._running = False
        if self._sender:
            self._sender.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
