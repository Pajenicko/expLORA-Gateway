#!/usr/bin/env python3
"""
Integration smoke test for an expLORA Gateway device (Lite or Pro).

Two operating modes:

1. Config-file mode (recommended for repeat runs)
   --------------------------------------------
   Reads everything from an INI file: home WiFi credentials, sensor key,
   sensor SN, gateway IP, serial port. If the gateway's IP from the config
   responds, the script jumps straight to the test phase. If not, the script
   provisions the device automatically:
     - reads UART to discover the gateway's AP SSID
     - (macOS) switches WiFi to that AP via `networksetup`
       (non-macOS) prints instructions and waits for ENTER
     - POSTs the WiFi credentials to /config at 192.168.4.1
     - watches UART for "WiFi connected! IP: x.x.x.x" after the reboot
     - switches back to the home WiFi
     - runs the full HTTP test suite against the discovered IP

   Usage:
       cp tests/integration/config.example.ini tests/integration/config.ini
       # fill in WiFi creds + sensor SN/key in config.ini
       python3 tests/integration/test_gateway.py --config tests/integration/config.ini

2. Legacy CLI-args mode (no config file)
   --------------------------------------
   All values via flags, gateway assumed already on the home network.
   No provisioning, no UART, no WiFi switching.

       python3 tests/integration/test_gateway.py --url http://192.168.1.42
       python3 tests/integration/test_gateway.py --url http://192.168.1.42 \\
           --test-sensor-sn 475481 --test-sensor-key D43C2780

Requirements:
    pip install requests pyserial

Exit codes:
    0   all tests passed
    1   at least one test failed
    2   could not connect to device, missing requirement, or provisioning failed
"""

import argparse
import configparser
import os
import platform
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional

try:
    import requests
except ImportError:
    print("ERROR: this script needs 'requests'. Run:  pip install requests")
    sys.exit(2)

# pyserial is only required when the config-file flow needs UART access.
# Import lazily so the CLI-args legacy mode still works without it.
try:
    import serial  # noqa: F401
    import serial.tools.list_ports as list_ports
    HAVE_PYSERIAL = True
except ImportError:
    HAVE_PYSERIAL = False

# --------------------------------------------------------------------- output

# (also used by Config / SerialMonitor / provisioning — defined first so they
# can colour their own log lines)
# Globally mutable so --no-color can blank them in one place
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
DIM = "\033[2m"
RESET = "\033[0m"
BOLD = "\033[1m"


def disable_colors():
    global GREEN, RED, YELLOW, CYAN, DIM, RESET, BOLD
    GREEN = RED = YELLOW = CYAN = DIM = RESET = BOLD = ""


# ====================================================================== config

@dataclass
class Config:
    """Parsed INI config. Optional fields stay empty strings if absent."""
    # [gateway]
    ip: str = ""
    ap_ssid_prefix: str = "expLORA-GW-"
    serial_port: str = ""
    serial_baud: int = 115200
    wifi_interface: str = "en0"
    admin_password: str = ""
    # [wifi]
    home_ssid: str = ""
    home_password: str = ""
    timezone: str = "Europe/Prague"
    # [test_sensor]
    sensor_sn: str = ""
    sensor_key: str = ""
    sensor_type: int = 1
    sensor_name: str = "integration-test"
    # [options]
    packet_timeout: int = 1200
    skip_reboot: bool = False
    # [flash]
    flash_before_test: bool = False
    rebuild_before_flash: bool = False
    erase_before_flash: bool = False
    pio_env: str = "lite"


def load_config(path: str) -> Config:
    if not os.path.exists(path):
        print(f"{RED}ERROR:{RESET} config file not found: {path}")
        print(f"Hint: cp tests/integration/config.example.ini {path}  and edit it.")
        sys.exit(2)

    parser = configparser.ConfigParser()
    parser.read(path)
    cfg = Config()

    if parser.has_section("gateway"):
        cfg.ip             = parser.get("gateway", "ip", fallback="").strip()
        cfg.ap_ssid_prefix = parser.get("gateway", "ap_ssid_prefix", fallback="expLORA-GW-").strip()
        cfg.serial_port    = parser.get("gateway", "serial_port", fallback="").strip()
        cfg.serial_baud    = parser.getint("gateway", "serial_baud", fallback=115200)
        cfg.wifi_interface = parser.get("gateway", "wifi_interface", fallback="en0").strip()
        cfg.admin_password = parser.get("gateway", "admin_password", fallback="").strip()
    if parser.has_section("wifi"):
        cfg.home_ssid     = parser.get("wifi", "ssid", fallback="").strip()
        cfg.home_password = parser.get("wifi", "password", fallback="").strip()
        cfg.timezone      = parser.get("wifi", "timezone", fallback=cfg.timezone).strip()
    if parser.has_section("test_sensor"):
        cfg.sensor_sn   = parser.get("test_sensor", "sn", fallback="").strip()
        cfg.sensor_key  = parser.get("test_sensor", "key", fallback="").strip()
        cfg.sensor_type = parser.getint("test_sensor", "type", fallback=1)
        cfg.sensor_name = parser.get("test_sensor", "name", fallback="integration-test").strip()
    if parser.has_section("options"):
        cfg.packet_timeout = parser.getint("options", "packet_timeout", fallback=1200)
        cfg.skip_reboot    = parser.getboolean("options", "skip_reboot", fallback=False)
    if parser.has_section("flash"):
        cfg.flash_before_test    = parser.getboolean("flash", "before_test", fallback=False)
        cfg.rebuild_before_flash = parser.getboolean("flash", "rebuild", fallback=False)
        cfg.erase_before_flash   = parser.getboolean("flash", "erase", fallback=False)
        cfg.pio_env              = parser.get("flash", "pio_env", fallback="lite").strip()

    if not cfg.home_ssid:
        print(f"{RED}ERROR:{RESET} [wifi].ssid is required in {path}")
        sys.exit(2)

    return cfg


# ============================================================== serial monitor

# ESP32-S3 USB VID/PIDs we'll happily autodetect.
#   0x303A:0x1001 — Espressif USB JTAG/serial (built into ESP32-S3)
#   0x10C4:0xEA60 — Silicon Labs CP210x (older boards)
#   0x1A86:0x55D4 — WCH CH343 / 0x7523 — CH340 (popular Chinese clones)
KNOWN_USB_VID_PID = {
    (0x303A, 0x1001),
    (0x10C4, 0xEA60),
    (0x1A86, 0x55D4),
    (0x1A86, 0x7523),
}


def autodetect_serial_port() -> Optional[str]:
    if not HAVE_PYSERIAL:
        return None
    candidates = []
    for p in list_ports.comports():
        if p.vid is not None and (p.vid, p.pid) in KNOWN_USB_VID_PID:
            candidates.append(p.device)
    if candidates:
        return candidates[0]
    # Fallback: any cu.usbmodem* on macOS or ttyACM* on Linux
    for p in list_ports.comports():
        name = p.device.lower()
        if "usbmodem" in name or "ttyacm" in name or "slab_usb" in name:
            return p.device
    return None


class SerialMonitor:
    """Reads UART in a background thread and exposes wait-for-pattern.

    Lines are pushed to a queue with their wall-clock timestamp so the
    operator can see the device's log as it streams.
    """
    def __init__(self, port: str, baud: int = 115200, echo: bool = True):
        if not HAVE_PYSERIAL:
            raise RuntimeError("pyserial not installed (pip install pyserial)")
        self.port = port
        self.baud = baud
        self.echo = echo
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._lines: "queue.Queue[str]" = queue.Queue()
        self._ser: Optional["serial.Serial"] = None  # type: ignore[name-defined]

    def start(self):
        self._ser = serial.Serial(self.port, self.baud, timeout=0.5)
        # Start reader FIRST — we want to catch the boot output that comes
        # next, whether triggered by opening the port (DTR pulse from the
        # OS) or by our explicit pulse_reset(). Any sleep here means we'd
        # lose those early lines.
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        info_(f"  serial monitor opened on {self.port} @ {self.baud}")

    def pulse_reset(self):
        """Pulse RTS to force a clean reset via the standard ESP32 auto-reset
        wiring (RTS → EN through a transistor). This guarantees we catch the
        boot output from the very first line, instead of joining mid-boot.

        No-op if DTR/RTS aren't accessible on this serial driver."""
        if not self._ser:
            return
        try:
            # Hold reset
            self._ser.rts = True   # EN low (reset asserted on standard wiring)
            self._ser.dtr = False  # IO0 high (normal boot, not bootloader)
            time.sleep(0.1)
            # Release reset — device boots and starts printing
            self._ser.rts = False
            info_("  pulsed reset via RTS")
        except Exception as e:
            info_(f"  could not pulse RTS: {e}")

    def stop(self):
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self._ser:
            try:
                self._ser.close()
            except Exception:
                pass

    def _reader(self):
        buf = b""
        assert self._ser is not None
        while not self._stop_event.is_set():
            try:
                chunk = self._ser.read(256)
            except Exception as e:
                info_(f"  serial read error: {e}")
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    text = line.decode("utf-8", errors="replace").rstrip("\r")
                except Exception:
                    text = repr(line)
                if self.echo:
                    print(f"{DIM}    serial: {text}{RESET}")
                self._lines.put(text)

    def wait_for(self, pattern: str, timeout: float, label: str = "") -> Optional[re.Match]:
        """Block until a line matches `pattern` (regex) or `timeout` elapses."""
        deadline = time.time() + timeout
        prog = re.compile(pattern)
        if label:
            info_(f"  waiting up to {int(timeout)}s for: {label}")
        while time.time() < deadline:
            try:
                line = self._lines.get(timeout=0.5)
            except queue.Empty:
                continue
            m = prog.search(line)
            if m:
                return m
        return None

    def drain(self):
        """Discard any queued lines (useful before triggering an event)."""
        while True:
            try:
                self._lines.get_nowait()
            except queue.Empty:
                return


# =============================================================== wifi switching

def _macos_power_cycle_wifi(interface: str) -> None:
    """Turn the WiFi interface off and back on. Forces macOS to re-scan
    visible networks within ~3 s — the cache otherwise refreshes only
    every 30 s or so, which is too slow when we're chasing an AP that
    just appeared a moment ago."""
    info_(f"  power-cycling {interface} to force a fresh WiFi scan …")
    subprocess.run(["networksetup", "-setairportpower", interface, "off"], check=False)
    time.sleep(2)
    subprocess.run(["networksetup", "-setairportpower", interface, "on"], check=False)
    time.sleep(4)  # let it re-scan


def wifi_switch_macos(interface: str, ssid: str, password: str = "") -> bool:
    """Switch the named macOS interface to the given SSID. Empty password = open net.

    Retries with backoff if macOS hasn't noticed the AP yet. After two
    failed attempts, power-cycles the WiFi adapter to force an immediate
    scan refresh — necessary when we're trying to join an AP that started
    broadcasting only seconds ago.
    """
    cmd = ["networksetup", "-setairportnetwork", interface, ssid]
    if password:
        cmd.append(password)
    safe_cmd = cmd if not password else cmd[:-1] + ["<password>"]

    # Total budget: ~35 s. macOS occasionally takes that long to surface a
    # freshly-started AP in its scan cache.
    backoffs = [0, 4, 6, 8, 10]
    bad_phrases = ("could not find", "could not", "failed", "error", "unable")
    for attempt, wait in enumerate(backoffs, start=1):
        if wait:
            time.sleep(wait)
        info_(f"  $ {' '.join(safe_cmd)}  (attempt {attempt}/{len(backoffs)})")
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            info_("  networksetup timed out — retrying")
            continue
        output = (result.stdout + result.stderr).strip()
        if output:
            info_(f"  {output}")
        # networksetup exits 0 even when it failed to find the network; the
        # textual output is the only reliable signal.
        if not any(p in output.lower() for p in bad_phrases):
            time.sleep(4)  # let association + DHCP settle
            return True
        # Halfway through the retry budget, force a fresh scan. We only do
        # this once — it briefly disconnects the current WiFi, which we'd
        # rather not do twice.
        if attempt == 2:
            _macos_power_cycle_wifi(interface)

    print(f"{RED}networksetup gave up after {len(backoffs)} attempts (~35 s) — AP not visible to macOS{RESET}")
    return False


def wifi_switch_prompt(ssid: str, password: str = ""):
    """Cross-platform fallback: tell the user what to do, wait for confirmation."""
    if password:
        print(f"  {YELLOW}>>{RESET} Please connect this computer to WiFi {BOLD}{ssid!r}{RESET} (password from the [wifi] section of your config)")
    else:
        print(f"  {YELLOW}>>{RESET} Please connect this computer to the open WiFi {BOLD}{ssid!r}{RESET}")
    input(f"  Press {BOLD}ENTER{RESET} once connected … ")


def wifi_switch(interface: str, ssid: str, password: str = "") -> bool:
    """Dispatch to OS-specific implementation; fall back to prompts."""
    if platform.system() == "Darwin" and shutil.which("networksetup"):
        return wifi_switch_macos(interface, ssid, password)
    wifi_switch_prompt(ssid, password)
    return True


# ============================================================ flash before run

def find_pio() -> Optional[str]:
    """Locate the PlatformIO CLI binary across the usual install locations."""
    candidates = [
        os.path.expanduser("~/.platformio/penv/bin/pio"),
        os.path.expanduser("~/.platformio/penv/Scripts/pio.exe"),  # Windows
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return shutil.which("pio")


def find_project_root() -> str:
    """Walk up from this file until platformio.ini turns up."""
    here = os.path.dirname(os.path.abspath(__file__))
    while here and here != os.path.dirname(here):
        if os.path.exists(os.path.join(here, "platformio.ini")):
            return here
        here = os.path.dirname(here)
    raise RuntimeError("platformio.ini not found in any parent directory")


def flash_device(env: str, rebuild: bool, erase: bool) -> None:
    """Run pio to (optionally rebuild +) flash the device. Streams output.

    Raises subprocess.CalledProcessError on any failure so the caller can
    decide to bail or continue.
    """
    pio = find_pio()
    if not pio:
        raise RuntimeError(
            "pio binary not found — expected at ~/.platformio/penv/bin/pio or on PATH. "
            "Install PlatformIO (https://platformio.org/install/cli)."
        )
    root = find_project_root()
    print(f"\n{BOLD}{CYAN}== Flashing firmware ({env}){RESET}")
    info_(f"  project root: {root}")
    info_(f"  pio binary:   {pio}")

    if rebuild:
        print(f"\n{CYAN}-- pio run -e {env}  (compile){RESET}")
        subprocess.check_call([pio, "run", "-e", env], cwd=root)

    if erase:
        print(f"\n{CYAN}-- pio run -e {env} -t erase  (wipe flash, forces re-provisioning){RESET}")
        subprocess.check_call([pio, "run", "-e", env, "-t", "erase"], cwd=root)

    print(f"\n{CYAN}-- pio run -e {env} -t upload  (flash){RESET}")
    subprocess.check_call([pio, "run", "-e", env, "-t", "upload"], cwd=root)

    # Give the chip a couple of seconds to come out of reset before we open
    # the serial port (avoids racing pio's own auto-reset sequence).
    time.sleep(2)
    info_(f"  {GREEN}✓ flash complete{RESET}")


# =================================================================== provisioning

# Regex patterns matching the firmware's serial log. We try several places
# the SSID can show up because exact timing relative to when we start
# reading is fragile — whichever line we catch first wins.
RE_BOOT_BANNER = r"expLORA Gateway \w+ starting up"  # Lite / Pro
RE_STA_IP      = r"WiFi connected[^!]*! IP: (\d+\.\d+\.\d+\.\d+)"

# Each of these has the SSID as the first capture group. Order matters
# only for diagnostic logging — any match is good.
AP_PATTERNS = (
    (r"AP started with SSID: (\S+?)[,\s]",            "main.cpp AP-only"),
    (r"Temporary AP started with SSID: (\S+?)[,\s]",  "main.cpp AP+STA"),
    (r"Starting AP mode: (\S+)",                      "WebPortal::setupAP entry"),
    (r"Setting up AP: (\S+)",                         "WebPortal::setupAP middle"),
    (r"AP brought up additively: (\S+?) @",           "WebPortal::ensureAPUp"),
)


def _strip_ssid(s: str) -> str:
    """Remove trailing punctuation we might have grabbed via (\\S+)."""
    return s.rstrip(",.;: ")


def scan_wifi_for_ap_macos(prefix: str, timeout: float = 30) -> Optional[str]:
    """Use system_profiler to look for a visible WiFi network whose SSID
    starts with `prefix`. Works without sudo, but the list is only as fresh
    as the most recent macOS WiFi scan — so we retry a few times.
    """
    deadline = time.time() + timeout
    pattern = re.compile(rf"\b{re.escape(prefix)}[A-Fa-f0-9]+\b")
    info_(f"  scanning visible WiFi networks for prefix {prefix!r} (up to {int(timeout)}s)")
    while time.time() < deadline:
        try:
            out = subprocess.check_output(
                ["system_profiler", "SPAirPortDataType"],
                text=True, timeout=15,
            )
            m = pattern.search(out)
            if m:
                return m.group(0)
        except subprocess.SubprocessError:
            pass
        time.sleep(3)
    return None


def discover_ap_ssid(monitor: SerialMonitor, prefix: str, timeout: float = 20) -> Optional[str]:
    """Try, in order: serial log scrape, macOS WiFi scan, give up.

    `monitor` may be partway through the device's boot output — we accept
    any of several known log lines that carry the SSID. If serial yields
    nothing within `timeout`, fall back to the system-level WiFi scan.
    """
    info_(f"  reading serial for AP SSID announcement (up to {int(timeout)}s) …")
    combined = "|".join(f"(?:{p})" for p, _ in AP_PATTERNS)
    m = monitor.wait_for(combined, timeout=timeout)
    if m:
        # Find which group actually matched (the first non-None capture)
        for grp in m.groups():
            if grp:
                ssid = _strip_ssid(grp)
                info_(f"  serial says AP SSID = {ssid!r}")
                return ssid

    info_("  serial log didn't surface the SSID — falling back to WiFi scan")
    if platform.system() == "Darwin":
        ssid = scan_wifi_for_ap_macos(prefix)
        if ssid:
            info_(f"  WiFi scan found {ssid!r}")
            return ssid

    # Last resort: ask the user
    print(f"\n  {YELLOW}>>{RESET} Could not auto-discover the gateway AP SSID.")
    print(f"     Open WiFi list, look for a network starting with {BOLD}{prefix}{RESET},")
    print(f"     and paste the full name here (or press ENTER to abort):")
    try:
        line = input("     SSID: ").strip()
    except EOFError:
        line = ""
    return line or None


def _http_alive(url: str, timeout: float = 3.0) -> bool:
    try:
        resp = requests.get(url, timeout=timeout)
        return resp.status_code == 200
    except requests.exceptions.RequestException:
        return False


def ensure_login(session: requests.Session, base: str, password: str) -> None:
    """Log the session in to the web interface.

    A gateway without an admin password (fresh flash, or updated from a
    version without login) accepts an empty password and then insists on
    setting one - the configured password is set in that case.
    """
    base = base.rstrip("/")
    if not password or len(password) < 8:
        raise RuntimeError("[gateway].admin_password (min. 8 characters) or --admin-password is required")
    resp = session.post(f"{base}/login", data={"password": password}, allow_redirects=False, timeout=10)
    if resp.status_code in (302, 303):
        return
    resp = session.post(f"{base}/login", data={"password": ""}, allow_redirects=False, timeout=10)
    if resp.status_code not in (302, 303) or "/password" not in resp.headers.get("Location", ""):
        raise RuntimeError(f"login failed (HTTP {resp.status_code}) - wrong admin password?")
    info_("  gateway has no admin password yet - setting the configured one")
    resp = session.post(f"{base}/password", data={"password": password, "confirm": password},
                        allow_redirects=False, timeout=10)
    if resp.status_code not in (302, 303):
        raise RuntimeError(f"setting the admin password failed (HTTP {resp.status_code})")


def provision_gateway(cfg: Config) -> str:
    """Walk the device through STA provisioning. Returns the discovered IP.

    Flow:
        1. Open serial monitor + pulse RTS for a clean reset
        2. Discover AP SSID — serial regex first, macOS WiFi scan
           fallback, user prompt as last resort
        3. Switch the host WiFi to that AP (macOS auto, prompt elsewhere)
        4. Verify we can reach 192.168.4.1
        5. POST /config with the home WiFi credentials
        6. Wait for the post-reboot banner …
        7. … then for "WiFi connected! IP: x.x.x.x"
        8. Switch host WiFi back to home, confirm reachability, return IP
    """
    print(f"\n{BOLD}{CYAN}== Provisioning gateway =={RESET}")

    port = cfg.serial_port or autodetect_serial_port()
    if not port:
        raise RuntimeError(
            "no serial port — set [gateway].serial_port in config.ini or plug "
            "an ESP32-S3 in via USB"
        )

    monitor = SerialMonitor(port, baud=cfg.serial_baud, echo=True)
    monitor.start()
    on_gateway_ap = False

    def switch_home():
        nonlocal on_gateway_ap
        if cfg.home_password:
            wifi_switch(cfg.wifi_interface, cfg.home_ssid, cfg.home_password)
        else:
            # Even for open home networks, the user just specified the SSID
            wifi_switch(cfg.wifi_interface, cfg.home_ssid)
        on_gateway_ap = False

    try:
        # 1. Trigger a clean reset so we know we're catching the boot
        # output from line one (not mid-way through it). On boards without
        # standard auto-reset wiring this is a no-op.
        monitor.pulse_reset()

        # 2. Discover the gateway's AP SSID. Tries serial log first, then
        # macOS WiFi scan, then prompts the user. Refuses to continue if
        # all three fail.
        ap_ssid = discover_ap_ssid(monitor, prefix=cfg.ap_ssid_prefix, timeout=20)
        if not ap_ssid:
            raise RuntimeError(
                "could not determine gateway AP SSID — aborting. Tip: leave "
                "the device powered on for 30+ s after flash so its AP shows "
                "up in macOS' WiFi list."
            )
        info_(f"  using AP SSID: {ap_ssid}")

        # 3. Switch host WiFi to the gateway AP
        on_gateway_ap = True
        if not wifi_switch(cfg.wifi_interface, ap_ssid):
            raise RuntimeError(f"failed to switch WiFi to {ap_ssid!r}")

        # 4. Confirm the AP is reachable
        if not _http_alive("http://192.168.4.1/firmware/version"):
            raise RuntimeError(
                "WiFi switched but http://192.168.4.1 unreachable — is the "
                "gateway actually broadcasting an AP?"
            )
        info_("  gateway AP confirmed reachable at 192.168.4.1")

        # 5. POST credentials. Don't worry about response — device reboots
        # ~1.5 s later (handleClient deferred restart) so the response may
        # not finish cleanly.
        info_("  logging in")
        session = requests.Session()
        ensure_login(session, "http://192.168.4.1", cfg.admin_password)

        info_("  POSTing /config (WiFi creds)")
        monitor.drain()
        try:
            session.post(
                "http://192.168.4.1/config",
                data={
                    "ssid": cfg.home_ssid,
                    "password": cfg.home_password,
                    "timezone": cfg.timezone,
                },
                timeout=5,
            )
        except requests.exceptions.RequestException as e:
            info_(f"  /config POST raised {e} — device is probably already restarting")

        # 6. Wait for the post-reboot banner (= confirms restart) and then
        # for the STA-connected line carrying the new IP
        m = monitor.wait_for(RE_BOOT_BANNER, timeout=15, label="post-reboot banner")
        if not m:
            raise RuntimeError("device did not reboot within 15 s of POST /config")

        m = monitor.wait_for(RE_STA_IP, timeout=30, label='"WiFi connected! IP: …"')
        if not m:
            raise RuntimeError(
                "device rebooted but did not join home WiFi within 30 s — "
                "check SSID / password in config"
            )
        new_ip = m.group(1)
        print(f"  {GREEN}✓ gateway is now on the home network at {BOLD}{new_ip}{RESET}")

        # 7. Switch host back to home WiFi
        info_("\n  switching host WiFi back to home network …")
        switch_home()

        # 8. Confirm we can reach the device on the home network
        info_("  confirming gateway is reachable from host …")
        ok_deadline = time.time() + 15
        while time.time() < ok_deadline:
            if _http_alive(f"http://{new_ip}/firmware/version"):
                info_(f"  reachable at http://{new_ip}")
                return new_ip
            time.sleep(1)
        raise RuntimeError(
            f"host switched WiFi but http://{new_ip} not reachable yet — try again in a moment"
        )

    finally:
        monitor.stop()
        # Never leave the host on the gateway AP after an error
        if on_gateway_ap:
            info_("\n  provisioning failed - switching host WiFi back to home network …")
            switch_home()


def pass_(name: str, detail: str = ""):
    msg = f"  {GREEN}[PASS]{RESET} {name}"
    if detail:
        msg += f"  {DIM}{detail}{RESET}"
    print(msg)


def fail_(name: str, detail: str = ""):
    msg = f"  {RED}[FAIL]{RESET} {name}"
    if detail:
        msg += f"  {DIM}{detail}{RESET}"
    print(msg)


def info_(line: str):
    print(f"{DIM}{line}{RESET}")


def section(title: str):
    print(f"\n{BOLD}{CYAN}== {title} =={RESET}")


# --------------------------------------------------------------- assertions

class TestFailed(Exception):
    pass


def assert_eq(expected, actual, label: str):
    if expected != actual:
        raise TestFailed(f"{label}: expected {expected!r}, got {actual!r}")


def assert_in(needle: str, haystack: str, label: str):
    if needle not in haystack:
        raise TestFailed(f"{label}: did not find {needle!r} in response")


def assert_status(resp, expected_codes: List[int]):
    if resp.status_code not in expected_codes:
        raise TestFailed(
            f"unexpected status {resp.status_code} (wanted one of {expected_codes})"
        )


# ------------------------------------------------------------------ runner

class Runner:
    def __init__(self, base_url: str, verbose: bool = False):
        self.base = base_url.rstrip("/")
        self.verbose = verbose
        self.failures: List[str] = []
        self.session = requests.Session()
        self.admin_password = ""
        # short timeout — device on local network should respond fast
        self.session.headers.update({"User-Agent": "explora-integration-test/1.0"})

    def run(self, name: str, fn: Callable):
        try:
            detail = fn()
            pass_(name, detail or "")
        except TestFailed as e:
            fail_(name, str(e))
            self.failures.append(name)
        except requests.exceptions.RequestException as e:
            fail_(name, f"transport error: {e}")
            self.failures.append(name)
        except Exception as e:  # noqa: BLE001
            fail_(name, f"unexpected exception: {e!r}")
            self.failures.append(name)

    def get(self, path: str, **kwargs) -> requests.Response:
        url = self.base + path
        if self.verbose:
            info_(f"  GET {url}")
        return self.session.get(url, timeout=kwargs.pop("timeout", 5), **kwargs)

    def post(self, path: str, data=None, **kwargs) -> requests.Response:
        url = self.base + path
        if self.verbose:
            info_(f"  POST {url}  data={data}")
        return self.session.post(
            url, data=data, timeout=kwargs.pop("timeout", 5), **kwargs
        )


# -------------------------------------------------------------- read-only

def t_firmware_version(r: Runner) -> str:
    resp = r.get("/firmware/version")
    assert_status(resp, [200])
    body = resp.json()
    if "version" not in body:
        raise TestFailed(f"no 'version' field in JSON: {body}")
    ver = body["version"]
    if not re.match(r"^\d+\.\d+\.\d+$", str(ver)):
        raise TestFailed(f"version {ver!r} is not semver-shaped")
    return f"v{ver}"


def t_api_json(r: Runner) -> str:
    resp = r.get("/api?format=json")
    assert_status(resp, [200])
    body = resp.json()
    for key in ("version", "time", "status", "sensors"):
        if key not in body:
            raise TestFailed(f"missing '{key}' in /api JSON: keys={list(body.keys())}")
    if not isinstance(body["sensors"], list):
        raise TestFailed("'sensors' is not a JSON array")
    return f"{len(body['sensors'])} sensors, status={body['status']!r}"


def t_api_csv(r: Runner) -> str:
    resp = r.get("/api?format=csv")
    assert_status(resp, [200])
    ct = resp.headers.get("Content-Type", "")
    if "csv" not in ct and "text" not in ct:
        raise TestFailed(f"unexpected Content-Type for CSV: {ct!r}")
    lines = resp.text.strip().splitlines()
    if not lines:
        raise TestFailed("empty CSV response")
    return f"{len(lines)} line(s)"


def t_sensors_page_empty(r: Runner) -> str:
    """When no sensors are configured the table doesn't render; we only get
    the heading and an empty-state message. Used when the live-sensor flow
    is disabled."""
    resp = r.get("/sensors")
    assert_status(resp, [200])
    html = resp.text
    assert_in("<h2>Sensors", html, "sensors page heading")
    assert_in("No sensors", html, "empty-state message")
    return "OK (empty state)"


def ui_mode(r: Runner) -> str:
    """'Simple' or 'Advanced' (the footer of every page names it)."""
    return "Advanced" if "Advanced mode</footer>" in r.get("/password").text else "Simple"


def set_ui_mode(r: Runner, mode: str) -> None:
    resp = r.post("/system/mode", data={"mode": mode.lower()}, allow_redirects=False)
    assert_status(resp, [302, 303])


def t_sensors_page_with_columns(r: Runner) -> str:
    """When at least one sensor exists the table renders with the Status
    column in both modes; the 24h column (health window) in Advanced only."""
    before = ui_mode(r)
    try:
        set_ui_mode(r, "Simple")
        html = r.get("/sensors").text
        assert_in("<h2>Sensors", html, "sensors page heading (Simple)")
        assert_in("<th>Status</th>", html, "Status column header (Simple)")
        if "<th>24h</th>" in html:
            raise TestFailed("24h column in Simple mode")
        set_ui_mode(r, "Advanced")
        html = r.get("/sensors").text
        assert_in("<th>Status</th>", html, "Status column header (Advanced)")
        assert_in("<th>24h</th>", html, "24h column header (Advanced)")
    finally:
        set_ui_mode(r, before)
    return f"OK in both modes (left in {before})"


def t_assets_public_cached(r: Runner) -> str:
    """/a.css and /a.js: no login, cached for a year (versioned URL)."""
    for path, kind in (("/a.css", "css"), ("/a.js", "javascript")):
        resp = requests.get(r.base + path + "?v=1", allow_redirects=False, timeout=5)
        assert_status(resp, [200])
        if kind not in resp.headers.get("Content-Type", ""):
            raise TestFailed(f"{path}: Content-Type {resp.headers.get('Content-Type')!r}")
        if "max-age=31536000" not in resp.headers.get("Cache-Control", ""):
            raise TestFailed(f"{path}: Cache-Control {resp.headers.get('Cache-Control')!r}")
    return "OK"


def t_api_gen2_rows(r: Runner) -> str:
    """Generation 2 sensors in /api: same keys as v1, "gen":2, the DevID as
    serialNumber, typeName GEN2 (skipped without a gen 2 sensor)."""
    body = r.get("/api?format=json").json()
    gen2 = [s for s in body["sensors"] if s.get("gen") == 2]
    if not gen2:
        return "no gen 2 sensor on this gateway - skipped"
    for s in gen2:
        if not re.match(r"^[0-9A-F]{8}$", str(s.get("serialNumber", ""))) or s.get("typeName") != "GEN2":
            raise TestFailed(f"bad gen 2 row: {s}")
        for key in ("deviceType", "name", "batteryVoltage", "rssi", "lastSeen"):
            if key not in s:
                raise TestFailed(f"gen 2 row without {key!r}: {s}")
    one = r.get(f"/api?format=json&sensor={gen2[0]['serialNumber']}").json()["sensors"]
    if len(one) != 1 or one[0].get("gen") != 2:
        raise TestFailed(f"?sensor=<DevID> returned {one}")
    csv = r.get("/api?format=csv").text
    if gen2[0]["serialNumber"] not in csv:
        raise TestFailed("gen 2 DevID not in the CSV")
    return f"{len(gen2)} gen 2 row(s), e.g. {gen2[0]['serialNumber']}"


def t_simple_hides_advanced(r: Runner) -> str:
    """Simple mode shows no gen 2 radio / security internals and no Logs in
    the navigation; Advanced does (both modes, the mode restored)."""
    before = ui_mode(r)
    try:
        set_ui_mode(r, "Simple")
        pages = {p: r.get(p).text for p in ("/", "/sensors", "/firmware", "/mqtt")}
        ids = re.findall(r"devId=([0-9A-F]{8})", pages["/sensors"])
        if ids:
            pages["gen2"] = r.get(f"/sensors/gen2?devId={ids[0]}").text
        for path, html in pages.items():
            for word in ("devNonce", "MIC failed", "FCnt", "href='/logs'", "Firmware update over the air",
                         "Signed images only"):
                if word in html:
                    raise TestFailed(f"{word!r} on {path} in Simple mode")
        set_ui_mode(r, "Advanced")
        if "href='/logs'" not in r.get("/").text:
            raise TestFailed("no Logs entry in the Advanced navigation")
    finally:
        set_ui_mode(r, before)
    return f"OK (left in {before})"


def t_config_page(r: Runner) -> str:
    resp = r.get("/config")
    assert_status(resp, [200])
    html = resp.text.lower()
    assert_in("ssid", html, "ssid form field")
    assert_in("password", html, "password form field")
    return "OK"


def t_root(r: Runner) -> str:
    resp = r.get("/", allow_redirects=False)
    assert_status(resp, [200, 302, 303])
    return f"status={resp.status_code}"


def t_diag_heap(r: Runner) -> str:
    resp = r.get("/diag/heap")
    assert_status(resp, [200])
    # Don't assume JSON vs HTML — just look for the word "heap"
    if "heap" not in resp.text.lower():
        raise TestFailed("no 'heap' word in /diag/heap response")
    return "OK"


def t_logs_page(r: Runner) -> str:
    resp = r.get("/logs")
    assert_status(resp, [200])
    return "OK"


def t_unknown_url_returns_404(r: Runner) -> str:
    resp = r.get("/this/does/not/exist", allow_redirects=False)
    # 404 in STA mode, 302 to /config in AP captive mode — both acceptable
    if resp.status_code not in (404, 302, 303):
        raise TestFailed(f"unexpected status {resp.status_code}")
    return f"status={resp.status_code}"


def t_api_unknown_sensor(r: Runner) -> str:
    resp = r.get("/api?sensor=FFFFFF&format=json")
    # Accept any 2xx or 404 — both are sane semantics for "not found"
    if resp.status_code not in (200, 404):
        raise TestFailed(f"unexpected status {resp.status_code}")
    return f"status={resp.status_code}"


# --------------------------------------------------------- POST validation

def t_config_empty_ssid_rejected(r: Runner) -> str:
    resp = r.post("/config", data={"ssid": "", "password": "somepass"})
    assert_status(resp, [400])
    # Validation added in 1.1.6 — regression guard
    if "ssid" not in resp.text.lower() and "empty" not in resp.text.lower():
        raise TestFailed(f"400 returned but message unclear: {resp.text!r}")
    return "400 + clear message"


def t_config_short_password_rejected(r: Runner) -> str:
    resp = r.post("/config", data={"ssid": "test-ssid", "password": "1234"})
    assert_status(resp, [400])
    if "password" not in resp.text.lower():
        raise TestFailed(f"400 returned but message unclear: {resp.text!r}")
    return "400 + clear message"


def t_config_missing_params_rejected(r: Runner) -> str:
    resp = r.post("/config", data={"ssid": "only-ssid"})  # no password
    assert_status(resp, [400])
    return "400"


def t_config_too_long_ssid_rejected(r: Runner) -> str:
    resp = r.post(
        "/config", data={"ssid": "x" * 33, "password": "validpass123"}
    )
    assert_status(resp, [400])
    return "400 (>32 chars)"


# ---------------------------------------- live sensor (real LoRa packet)

def _find_sensor(api_body: dict, sn_hex: str) -> Optional[dict]:
    """Return the sensor dict from /api JSON whose serialNumber matches sn_hex (case-insensitive)."""
    want = sn_hex.upper().lstrip("0")
    for s in api_body.get("sensors", []):
        got = str(s.get("serialNumber", "")).upper().lstrip("0")
        if got == want:
            return s
    return None


def t_add_or_verify_test_sensor(
    r: Runner, sn: str, key: str, name: str, sensor_type: int
) -> str:
    """Add the test sensor if it's not yet configured. Idempotent across reruns."""
    # 1. existence check via /api
    resp = r.get(f"/api?sensor={sn}&format=json")
    assert_status(resp, [200])
    existing = _find_sensor(resp.json(), sn)
    if existing is not None:
        return f"sensor {sn} already configured (name={existing.get('name')!r})"

    # 2. POST add
    resp = r.post(
        "/sensors/add",
        data={
            "name": name,
            "deviceType": str(sensor_type),
            "serialNumber": sn,
            "deviceKey": key,
        },
    )
    # The handler responds 200 + HTML redirect on success
    if resp.status_code not in (200, 302, 303):
        raise TestFailed(f"add returned status {resp.status_code}: {resp.text[:120]!r}")

    # 3. verify visible in /api
    resp = r.get(f"/api?sensor={sn}&format=json")
    if _find_sensor(resp.json(), sn) is None:
        raise TestFailed(f"sensor {sn} not visible in /api after add")
    return f"sensor {sn} added as {name!r}"


def t_wait_for_packet(
    r: Runner, sn: str, timeout_s: int, poll_interval_s: int = 30
) -> str:
    """Poll /api until lastSeen indicates the gateway received a fresh packet.

    lastSeen is reported as "seconds since the packet arrived" (or -1 if the
    sensor has never been seen). We consider lastSeen < 2*poll_interval a
    fresh packet — within one poll window after it actually landed.
    """
    started = time.time()
    deadline = started + timeout_s
    fresh_threshold = poll_interval_s * 2
    info_(
        f"  polling /api every {poll_interval_s}s for up to {timeout_s}s "
        f"(fresh = lastSeen < {fresh_threshold}s)"
    )
    while time.time() < deadline:
        elapsed = int(time.time() - started)
        sensor = None
        ls_repr = "?"
        try:
            resp = r.get(f"/api?sensor={sn}&format=json", timeout=5)
            sensor = _find_sensor(resp.json(), sn)
            if sensor is not None:
                ls = sensor.get("lastSeen", -1)
                ls_repr = str(ls)
                if isinstance(ls, (int, float)) and 0 <= ls < fresh_threshold:
                    return (
                        f"packet received after {elapsed}s "
                        f"(lastSeen={ls}s, temp={sensor.get('temperature')}, "
                        f"hum={sensor.get('humidity')}, press={sensor.get('pressure')}, "
                        f"batt={sensor.get('batteryVoltage')}V)"
                    )
        except requests.exceptions.RequestException as e:
            ls_repr = f"req error: {e}"

        info_(f"    [t={elapsed:4d}s] lastSeen={ls_repr}")
        time.sleep(poll_interval_s)

    raise TestFailed(
        f"no fresh packet from sensor {sn} within {timeout_s}s — sensor offline, "
        f"out of range, or wrong key?"
    )


def t_validate_decoded_values(r: Runner, sn: str, sensor_type: int) -> str:
    """Verify the gateway decoded the received packet into physically plausible values.

    This is the end-to-end signal that decrypt + parse + calibration pipeline
    is working on real hardware (the native unit tests cover correctness in
    isolation; this one catches integration regressions).
    """
    resp = r.get(f"/api?sensor={sn}&format=json")
    sensor = _find_sensor(resp.json(), sn)
    if sensor is None:
        raise TestFailed(f"sensor {sn} not in /api response")

    issues: List[str] = []

    # Battery is reported for every sensor type
    batt = sensor.get("batteryVoltage", 0)
    if not (0.5 <= float(batt) <= 5.0):
        issues.append(f"batteryVoltage {batt} V out of (0.5, 5.0)")

    # Per-type range checks. Liberal bounds — we're catching "wildly wrong"
    # not "minor calibration drift".
    if sensor_type == 1:  # BME280
        temp = sensor.get("temperature", 0)
        if not (-40.0 <= float(temp) <= 80.0):
            issues.append(f"temperature {temp} °C outside (-40, 80)")
        hum = sensor.get("humidity", 0)
        if not (0.0 <= float(hum) <= 100.0):
            issues.append(f"humidity {hum}% outside (0, 100)")
        press = sensor.get("pressure", 0)
        if not (850.0 <= float(press) <= 1100.0):
            issues.append(f"pressure {press} hPa outside (850, 1100)")
    elif sensor_type == 2:  # SCD40
        temp = sensor.get("temperature", 0)
        if not (-40.0 <= float(temp) <= 80.0):
            issues.append(f"temperature {temp} °C outside (-40, 80)")
        ppm = sensor.get("ppm", 0)
        if not (300.0 <= float(ppm) <= 10000.0):
            issues.append(f"CO2 {ppm} ppm outside (300, 10000)")

    if issues:
        raise TestFailed("; ".join(issues))

    # Build a friendly summary based on what fields are populated
    parts = []
    for k in ("temperature", "humidity", "pressure", "ppm", "lux"):
        if k in sensor and sensor[k] != 0:
            parts.append(f"{k}={sensor[k]}")
    parts.append(f"batt={sensor.get('batteryVoltage')}V")
    parts.append(f"rssi={sensor.get('rssi')}dBm")
    return ", ".join(parts)


def t_sensor_visible_in_sensors_page(r: Runner, sn: str) -> str:
    """Verify the sensor's SN shows up on the /sensors HTML page (the ID
    column of the Advanced mode; Simple shows no serial numbers)."""
    before = ui_mode(r)
    try:
        set_ui_mode(r, "Advanced")
        resp = r.get("/sensors")
    finally:
        set_ui_mode(r, before)
    assert_status(resp, [200])
    sn_upper = sn.upper().lstrip("0")
    # The HTML uses formatSN which uppercases and zero-pads to 6 chars
    if sn_upper not in resp.text.upper():
        raise TestFailed(f"SN {sn_upper} not present in /sensors HTML (Advanced)")
    return f"SN {sn_upper} visible on /sensors page (Advanced)"


def t_sensor_health_badge_after_packet(r: Runner, sn: str) -> str:
    """After a successful packet, /sensors must render the Healthy badge for
    this sensor and the 24h column must show at least one OK count. This is
    the end-to-end signal that the 1.1.11 health-tracking pipeline is
    actually firing (recordSensorSuccess → render in HTML)."""
    resp = r.get("/sensors")
    assert_status(resp, [200])
    html = resp.text
    # renderHealthBadge(): `<span class=ok>OK</span>` for a healthy sensor
    if "<span class=ok>OK</span>" not in html:
        raise TestFailed(
            "no 'OK' status on /sensors — recordSensorSuccess didn't fire?"
        )
    return "OK status present in HTML"


# ----------------------------------------------------------------- login

def t_login_required(r: Runner) -> str:
    resp = requests.get(r.base + "/sensors", allow_redirects=False, timeout=5)
    assert_status(resp, [302, 303])
    location = resp.headers.get("Location", "")
    if "/login" not in location:
        raise TestFailed(f"redirect to {location!r}, expected /login")
    resp = requests.post(r.base + "/sensors/add", data={"name": "x"}, allow_redirects=False, timeout=5)
    assert_status(resp, [401])
    return "OK"


def t_api_public(r: Runner) -> str:
    resp = requests.get(r.base + "/api?format=json", allow_redirects=False, timeout=5)
    assert_status(resp, [200])
    return "OK"


def t_wrong_password_rejected(r: Runner) -> str:
    resp = requests.post(r.base + "/login", data={"password": "definitely-wrong-password"},
                         allow_redirects=False, timeout=10)
    assert_status(resp, [401])
    return "OK"


# --------------------------------------------------------------- reboot

def t_reboot_round_trip(r: Runner, current_version: str) -> str:
    """Best-effort reboot smoke test.

    POST /reboot, wait for the device to drop off, poll until it comes
    back, then verify it's serving requests again with the same version.
    """
    # 1) snapshot pre-reboot
    info_(f"  pre-reboot version: {current_version}")

    # 2) issue reboot. The endpoint may not return 200 (device restarts
    # mid-response), so we tolerate connection errors here.
    try:
        resp = r.get("/reboot", timeout=3)
        info_(f"  /reboot returned {resp.status_code}")
    except requests.exceptions.RequestException as e:
        info_(f"  /reboot failed (expected — device rebooting): {e}")

    # 3) wait for device to go down (it might already be down). 20 s
    # is generous — the deferred-restart hook fires within ~1.5 s of the
    # request, plus a few seconds for the WiFi stack to actually drop the
    # association. We've seen the original 10 s window be too short when
    # the main loop is busy with LoRa packet processing.
    info_("  waiting up to 20 s for device to drop off …")
    deadline = time.time() + 20
    went_down = False
    while time.time() < deadline:
        try:
            r.get("/firmware/version", timeout=1)
        except requests.exceptions.RequestException:
            went_down = True
            break
        time.sleep(0.5)
    if not went_down:
        raise TestFailed(
            "/reboot did not cause the device to drop off within 20 s "
            "(endpoint stayed responsive) — handleReboot may not have run"
        )

    # 4) wait up to 60 s for device to come back
    info_("  waiting up to 60 s for device to come back …")
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            resp = r.get("/firmware/version", timeout=2)
            if resp.status_code == 200:
                body = resp.json()
                returned = body.get("version")
                if returned == current_version:
                    # Sessions live in RAM - log in again for later tests
                    ensure_login(r.session, r.base, r.admin_password)
                    return f"back after reboot, version={returned}"
                else:
                    raise TestFailed(
                        f"version changed across reboot: pre={current_version} post={returned}"
                    )
        except requests.exceptions.RequestException:
            pass
        time.sleep(1)
    raise TestFailed("device did not come back within 60 s")


# ----------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    # In config-file mode, --url is derived from [gateway].ip (or auto-discovered).
    # In legacy mode, --url is required.
    parser.add_argument(
        "--config",
        help="INI config file (recommended). When set, --url becomes optional "
        "and the script can auto-provision via UART + WiFi switching.",
    )
    parser.add_argument(
        "--url",
        help="Base URL of the gateway (legacy mode). Ignored when --config is given "
        "and the configured IP responds.",
    )
    parser.add_argument(
        "--skip-reboot",
        action="store_true",
        help="Skip the reboot smoke test (the only test that disrupts the device)",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Print each request URL"
    )
    parser.add_argument(
        "--no-color", action="store_true", help="Disable ANSI colour escape sequences"
    )
    # Live-sensor round-trip: only runs when --test-sensor-sn is provided.
    parser.add_argument(
        "--test-sensor-sn",
        help="Hex serial number of a real sensor to register and wait for "
        "(e.g. 475481). Enables the live LoRa packet test.",
    )
    parser.add_argument(
        "--test-sensor-key",
        help="Hex device key for --test-sensor-sn (e.g. D43C2780). Required if "
        "--test-sensor-sn is given.",
    )
    parser.add_argument(
        "--test-sensor-name",
        default="integration-test",
        help="Display name to give the test sensor (default: integration-test)",
    )
    parser.add_argument(
        "--test-sensor-type",
        type=int,
        default=1,
        help="Sensor type code: 1=CLIMA (BME280), 2=CARBON (SCD40), 3=METEO (default: 1)",
    )
    parser.add_argument(
        "--admin-password",
        default="",
        help="Web admin password (set on the gateway if it has none yet)",
    )
    parser.add_argument(
        "--packet-timeout",
        type=int,
        default=1200,
        help="Seconds to wait for the first LoRa packet (default: 1200 = 20 min)",
    )
    parser.add_argument(
        "--flash",
        action="store_true",
        help="Flash firmware via PlatformIO before testing. Preserves LittleFS, "
        "so saved WiFi config survives.",
    )
    parser.add_argument(
        "--rebuild",
        action="store_true",
        help="Run `pio run` to compile before flashing (implies --flash).",
    )
    parser.add_argument(
        "--erase",
        action="store_true",
        help="Also erase the entire flash before upload — wipes LittleFS, "
        "forcing re-provisioning. (implies --flash)",
    )
    args = parser.parse_args()

    if args.no_color:
        disable_colors()

    # ---------------------------------------------------------------- resolve
    # Resolve the inputs (config-file mode wins over CLI args where set).
    cfg: Optional[Config] = None
    if args.config:
        cfg = load_config(args.config)
        # Apply CLI overrides on top of the config
        sensor_sn       = args.test_sensor_sn or cfg.sensor_sn
        sensor_key      = args.test_sensor_key or cfg.sensor_key
        sensor_name     = args.test_sensor_name or cfg.sensor_name
        sensor_type     = args.test_sensor_type if args.test_sensor_type != 1 else cfg.sensor_type
        packet_timeout  = args.packet_timeout if args.packet_timeout != 1200 else cfg.packet_timeout
        skip_reboot     = args.skip_reboot or cfg.skip_reboot
    else:
        # Legacy CLI-args mode: --url is required
        if not args.url:
            print(f"{RED}ERROR:{RESET} either --config or --url is required")
            return 2
        sensor_sn      = args.test_sensor_sn
        sensor_key     = args.test_sensor_key
        sensor_name    = args.test_sensor_name
        sensor_type    = args.test_sensor_type
        packet_timeout = args.packet_timeout
        skip_reboot    = args.skip_reboot

    if sensor_sn and not sensor_key:
        print(f"{RED}ERROR:{RESET} sensor key required when SN is set (CLI: --test-sensor-key, config: [test_sensor].key)")
        return 2

    # ---------------------------------------------------------------- flash
    # CLI flags win; --rebuild/--erase imply --flash. Config provides defaults.
    flash_now   = args.flash or args.rebuild or args.erase or (cfg.flash_before_test if cfg else False)
    rebuild_now = args.rebuild or (cfg.rebuild_before_flash if cfg else False)
    erase_now   = args.erase or (cfg.erase_before_flash if cfg else False)
    pio_env     = cfg.pio_env if cfg else "lite"
    if flash_now:
        try:
            flash_device(pio_env, rebuild=rebuild_now, erase=erase_now)
        except subprocess.CalledProcessError as e:
            print(f"\n{RED}ERROR:{RESET} pio command failed (exit {e.returncode}). See output above.")
            return 2
        except Exception as e:  # noqa: BLE001
            print(f"\n{RED}ERROR during flash:{RESET} {e}")
            return 2

    # ---------------------------------------------------------------- resolve URL
    # Pick a URL: (a) CLI --url wins, (b) [gateway].ip from config if reachable,
    # (c) otherwise run provisioning (UART + WiFi switch) to discover one.
    url: Optional[str] = args.url
    if not url and cfg:
        if cfg.ip:
            candidate = f"http://{cfg.ip}"
            info_(f"trying configured gateway IP: {candidate}")
            if _http_alive(f"{candidate}/firmware/version"):
                url = candidate
            else:
                info_(f"  not reachable — provisioning needed")
        if not url:
            try:
                discovered = provision_gateway(cfg)
                url = f"http://{discovered}"
            except Exception as e:  # noqa: BLE001
                print(f"\n{RED}ERROR during provisioning:{RESET} {e}")
                return 2

    if not url:
        print(f"{RED}ERROR:{RESET} could not determine gateway URL")
        return 2

    r = Runner(url, verbose=args.verbose)
    r.admin_password = args.admin_password or (cfg.admin_password if cfg else "")

    # Sanity ping
    try:
        resp = r.get("/firmware/version", timeout=3)
        body = resp.json()
        current_version = body.get("version", "unknown")
    except Exception as e:  # noqa: BLE001
        print(f"{RED}ERROR:{RESET} cannot reach {url}/firmware/version  ({e})")
        print("Is the device powered on and on the same network?")
        return 2

    print(f"\n{BOLD}Testing gateway at {url}  (firmware v{current_version}){RESET}")

    section("Login")
    r.run("pages require a login", lambda: t_login_required(r))
    r.run("/api is public", lambda: t_api_public(r))
    r.run("wrong password rejected", lambda: t_wrong_password_rejected(r))
    try:
        ensure_login(r.session, r.base, r.admin_password)
        pass_("log in with the admin password")
    except RuntimeError as e:
        fail_("log in with the admin password", str(e))
        return 1

    # ----------------------------------------------- Phase 1: setup (mutates)
    # Adding the sensor first means downstream smoke tests see the table
    # rendered (so they can assert the new health columns are present), and
    # the live-sensor phase can immediately start polling /api for packets.
    if sensor_sn:
        section("Setup")
        r.run(
            "add or verify test sensor",
            lambda: t_add_or_verify_test_sensor(
                r, sensor_sn, sensor_key, sensor_name, sensor_type,
            ),
        )

    # ----------------------------------------------- Phase 2: read-only smoke
    section("Read-only smoke")
    r.run("firmware version JSON", lambda: t_firmware_version(r))
    r.run("/api?format=json shape", lambda: t_api_json(r))
    r.run("/api?format=csv basics", lambda: t_api_csv(r))
    r.run("/api gen 2 rows", lambda: t_api_gen2_rows(r))
    r.run("/a.css and /a.js public and cached", lambda: t_assets_public_cached(r))
    r.run("Simple mode hides the Advanced content", lambda: t_simple_hides_advanced(r))
    if sensor_sn:
        r.run("/sensors page (has health columns)", lambda: t_sensors_page_with_columns(r))
    else:
        r.run("/sensors page (empty state)", lambda: t_sensors_page_empty(r))
    r.run("/config form", lambda: t_config_page(r))
    r.run("/ (root)", lambda: t_root(r))
    r.run("/diag/heap", lambda: t_diag_heap(r))
    r.run("/logs", lambda: t_logs_page(r))
    r.run("unknown URL → 404", lambda: t_unknown_url_returns_404(r))
    r.run("/api with unknown sensor", lambda: t_api_unknown_sensor(r))

    # ----------------------------------------------- Phase 3: POST validation
    section("POST validation (added in 1.1.6)")
    r.run("empty SSID → 400", lambda: t_config_empty_ssid_rejected(r))
    r.run("password < 8 chars → 400", lambda: t_config_short_password_rejected(r))
    r.run("missing password param → 400", lambda: t_config_missing_params_rejected(r))
    r.run("SSID > 32 chars → 400", lambda: t_config_too_long_ssid_rejected(r))

    # ----------------------------------------------- Phase 4: live sensor wait
    if sensor_sn:
        section(
            f"Live sensor — wait + validate (SN {sensor_sn}, type "
            f"{sensor_type}, timeout {packet_timeout}s)"
        )
        r.run(
            f"wait for fresh LoRa packet (up to {packet_timeout}s)",
            lambda: t_wait_for_packet(r, sensor_sn, packet_timeout),
        )
        r.run(
            "decoded values are physically plausible",
            lambda: t_validate_decoded_values(r, sensor_sn, sensor_type),
        )
        r.run(
            "sensor visible on /sensors page",
            lambda: t_sensor_visible_in_sensors_page(r, sensor_sn),
        )
        r.run(
            "/sensors page shows the OK status after packet",
            lambda: t_sensor_health_badge_after_packet(r, sensor_sn),
        )
    else:
        info_("\nSkipping live sensor test (sensor SN not set in config or CLI)")

    if skip_reboot:
        info_("Skipping reboot test (skip_reboot=true)")
    else:
        section("Reboot recovery")
        r.run("device reboots and returns", lambda: t_reboot_round_trip(r, current_version))

    # ------------------------------------------------------------- summary
    print()
    if r.failures:
        print(f"{RED}{BOLD}FAILED:{RESET} {len(r.failures)} test(s)")
        for f in r.failures:
            print(f"  - {f}")
        return 1
    else:
        print(f"{GREEN}{BOLD}ALL PASSED{RESET}")
        return 0


if __name__ == "__main__":
    sys.exit(main())
