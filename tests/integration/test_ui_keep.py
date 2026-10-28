#!/usr/bin/env python3
"""
Host test: settings that the web forms do not post are kept.

The Simple interface leaves fields out (sensor type / serial / key, MQTT TLS
and topics), and the data-if toggler disables the inputs of a hidden part
(WiFi credentials while "Use WiFi" is off, LTE APN / PIN while LTE is off,
MQTT settings while it is off, static Ethernet addresses with DHCP) - a
disabled input is not posted. Every handler must keep the stored value of a
field that is absent. This script posts exactly what a browser posts in
those states and reads the stored values back from the Advanced pages.

Restores everything it changes (MQTT settings, sensor values, LTE APN, the
interface mode, the automatic updates card). The network part restarts the
gateway several times (each "Save and restart"); --no-network skips it.

    python3 tests/integration/test_ui_keep.py --url http://192.168.1.42 \\
        --admin-password ... [--no-network]

Exit codes: 0 all passed, 1 a check failed, 2 setup / connection error.
"""

import argparse
import html as htmllib
import re
import sys
import time

try:
    import requests
except ImportError:
    print("ERROR: this script needs 'requests' (pip install requests)")
    sys.exit(2)

FAILURES = []


def check(cond, what):
    print(("  PASS  " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


class Gw:
    def __init__(self, base, password):
        self.base = base.rstrip("/")
        self.password = password
        self.s = requests.Session()

    def login(self):
        r = self.s.post(self.base + "/login", data={"password": self.password}, allow_redirects=False, timeout=10)
        if r.status_code not in (302, 303):
            raise RuntimeError(f"login failed (HTTP {r.status_code})")

    def get(self, path):
        r = self.s.get(self.base + path, timeout=20)
        r.raise_for_status()
        return r.text

    def post(self, path, data, ok=(200, 302, 303)):
        r = self.s.post(self.base + path, data=data, allow_redirects=False, timeout=20)
        if r.status_code not in ok:
            raise RuntimeError(f"POST {path} -> HTTP {r.status_code}: {r.text[:200]}")
        return r

    def mode(self, m=None):
        if m:
            self.post("/system/mode", {"mode": m})
        return "advanced" if "Advanced mode</footer>" in self.get("/password") else "simple"

    def wait_restart(self, timeout=240):
        """After a "Save and restart": down, then up again (logged in anew)."""
        t0 = time.time()
        while time.time() - t0 < 30:
            try:
                requests.get(self.base + "/firmware/version", timeout=2)
                time.sleep(1)
            except requests.exceptions.RequestException:
                break
        while time.time() - t0 < timeout:
            try:
                if requests.get(self.base + "/firmware/version", timeout=3).status_code == 200:
                    time.sleep(5)
                    self.login()
                    return
            except requests.exceptions.RequestException:
                pass
            time.sleep(3)
        raise RuntimeError("the gateway did not come back after the restart")


def value(page, name):
    m = re.search(r"<input[^>]*\bname='?" + re.escape(name) + r"'?[^>]*>", page)
    if not m:
        return None
    v = re.search(r"value=(?:'([^']*)'|([^ '>]+))", m.group(0))
    return htmllib.unescape(v.group(1) if v.group(1) is not None else v.group(2)) if v else ""


def checked(page, name):
    m = re.search(r"<input[^>]*\bname='?" + re.escape(name) + r"'?[^>]*>", page)
    return bool(m) and " checked" in m.group(0)


def placeholder(page, name):
    m = re.search(r"<input[^>]*\bname='?" + re.escape(name) + r"'?[^>]*>", page)
    p = re.search(r"placeholder='([^']*)'", m.group(0)) if m else None
    return p.group(1) if p else None


def selected(page, name):
    m = re.search(r"<select[^>]*name=" + re.escape(name) + r"[^>]*>(.*?)</select>", page, re.S)
    o = re.search(r"<option value=([^ >]+) selected", m.group(1)) if m else None
    return o.group(1) if o else None


# ------------------------------------------------------------------ MQTT

def mqtt_read(gw):
    p = gw.get("/mqtt")
    return {k: value(p, k) for k in ("host", "port", "user", "prefix", "haPrefix")} | {
        "enabled": checked(p, "enabled"), "tls": checked(p, "tls"), "ha": checked(p, "haEnabled"),
        "pw": placeholder(p, "password")}


def mqtt_adv_post(gw, s):
    d = {"f": "1", "adv": "1", "host": s["host"], "port": s["port"], "user": s["user"], "password": "",
         "prefix": s["prefix"], "haPrefix": s["haPrefix"]}
    if s["enabled"]:
        d["enabled"] = "1"
    if s["tls"]:
        d["tls"] = "1"
    if s["ha"]:
        d["haEnabled"] = "1"
    gw.post("/mqtt", d)


def test_mqtt(gw):
    print("MQTT")
    gw.mode("advanced")
    orig = mqtt_read(gw)
    if orig["pw"]:
        print("  (a stored MQTT password exists - it is never typed here, so it stays)")
    test = dict(orig, enabled=False, host="keep-test.invalid", port="1884", tls=True, ha=True,
                prefix="keeptest", haPrefix="hakeep")
    if orig["pw"]:
        test["user"] = orig["user"]  # keeps the stored password
    try:
        mqtt_adv_post(gw, test)
        gw.mode("simple")
        # Simple, MQTT off: the hidden part is disabled - only the switch is posted (nothing)
        gw.post("/mqtt", {})
        gw.mode("advanced")
        got = mqtt_read(gw)
        check(got == dict(test, pw=orig["pw"]), f"MQTT off in Simple: every setting kept ({got})")
        # Simple, MQTT on: no TLS, root or discovery topic in the form
        gw.mode("simple")
        gw.post("/mqtt", {"enabled": "1", "f": "1", "host": test["host"], "port": test["port"],
                          "user": test["user"], "password": ""})
        gw.mode("advanced")
        got = mqtt_read(gw)
        check(got["tls"] and got["prefix"] == "keeptest" and got["haPrefix"] == "hakeep",
              "MQTT on in Simple: TLS and both topics kept")
        check(not got["ha"], "MQTT on in Simple: the posted discovery checkbox (off) applies")
        check(got["pw"] == orig["pw"], "MQTT password kept (placeholder)")
    finally:
        gw.mode("advanced")
        mqtt_adv_post(gw, orig)
        check(mqtt_read(gw) == orig, "MQTT settings restored")


# --------------------------------------------------------------- sensors

def test_sensors(gw):
    print("Sensors")
    gw.mode("advanced")
    sensors = gw.get("/sensors")
    m = re.search(r"/sensors/edit\?index=(\d+)", sensors)
    if m:
        idx = m.group(1)
        page = gw.get(f"/sensors/edit?index={idx}")
        sn = value(page, "sn")
        before = {k: value(page, k) for k in ("name", "serialNumber", "customUrl", "tempCorr", "humCorr",
                                               "pressCorr", "windSpeedCorr", "altitude")}
        before["type"] = selected(page, "deviceType")
        try:
            # Simple edit: no type, serial or key; the URL and corrections posted
            gw.mode("simple")
            simple = gw.get(f"/sensors/edit?index={idx}")
            check(value(simple, "serialNumber") is None and value(simple, "deviceKey") is None,
                  "Simple edit form has no serial number / key")
            data = {"index": idx, "sn": sn, "name": before["name"], "customUrl": "http://keep.test/x?t=*TEMP*"}
            for f in re.findall(r"name=(\w+Corr|altitude) ", simple):
                data[f] = value(simple, f)
            data["tempCorr"] = "0.5"
            gw.post("/sensors/update", data)
            gw.mode("advanced")
            after = gw.get(f"/sensors/edit?index={idx}")
            check(selected(after, "deviceType") == before["type"] and value(after, "serialNumber") == before["serialNumber"],
                  "v1 Simple edit: type and serial number kept")
            check(value(after, "customUrl") == "http://keep.test/x?t=*TEMP*", "v1 custom URL saved from Simple")
            check(value(after, "tempCorr") == "0.5", "v1 correction saved from Simple")
            others = [k for k in ("humCorr", "pressCorr", "windSpeedCorr", "altitude") if before[k] is not None]
            check(all(value(after, k) == before[k] for k in others), f"v1 other corrections kept ({others})")
            # Round trip: 0.125 is shown as it is, and saving the form unchanged keeps it
            full = {"index": idx, "sn": sn, "name": before["name"]}
            for f in re.findall(r"name=(\w+Corr|altitude) ", after):
                full[f] = value(after, f)
            gw.post("/sensors/update", dict(full, tempCorr="0.125"))
            page = gw.get(f"/sensors/edit?index={idx}")
            check(value(page, "tempCorr") == "0.125" and "step=any" in page, "v1 correction 0.125 shown exactly")
            full = {"index": idx, "sn": sn, "name": before["name"]}
            for f in re.findall(r"name=(\w+Corr|altitude) ", page):
                full[f] = value(page, f)
            gw.post("/sensors/update", full)
            page = gw.get(f"/sensors/edit?index={idx}")
            check(value(page, "tempCorr") == "0.125", "v1 unchanged save is lossless (0.125 kept)")
            # Text that is not a number: refused (400), nothing changed
            r = gw.post("/sensors/update", dict(full, tempCorr="abc"), ok=(400,))
            page = gw.get(f"/sensors/edit?index={idx}")
            check(r.status_code == 400 and "not a number" in r.text and value(page, "tempCorr") == "0.125",
                  "v1 correction 'abc' refused with 400, value kept")
        finally:
            restore = {"index": idx, "sn": sn, "name": before["name"], "deviceType": before["type"],
                       "serialNumber": before["serialNumber"], "customUrl": before["customUrl"] or "",
                       "tempCorr": before["tempCorr"]}
            gw.post("/sensors/update", restore)
            page = gw.get(f"/sensors/edit?index={idx}")
            check(value(page, "customUrl") == (before["customUrl"] or "") and value(page, "tempCorr") == before["tempCorr"],
                  "v1 sensor restored")
    else:
        print("  (no generation 1 sensor - skipped)")
    ids = re.findall(r"/sensors/gen2\?devId=([0-9A-F]{8})", sensors)
    if not ids:
        print("  (no generation 2 sensor - skipped)")
        return
    dev = ids[0]
    page = gw.get(f"/sensors/gen2?devId={dev}")
    name, interval = value(page, "name"), value(page, "interval")
    url = value(page, "customUrl") or ""
    corr = [f for f in re.findall(r"name=(\w+Corr|altitude) ", page)]
    before = {f: value(page, f) for f in corr}
    try:
        # Advanced card: name + corrections, no interval -> interval kept
        data = {"devId": dev, "name": name + "-k"}
        for f in corr:
            data[f] = before[f]
        if "tempCorr" in corr:
            data["tempCorr"] = "-0.3"
        gw.post("/sensors/gen2/edit", data)
        page = gw.get(f"/sensors/gen2?devId={dev}")
        check(value(page, "interval") == interval, "gen 2 name/corrections save keeps the report interval")
        check(value(page, "name") == name + "-k", "gen 2 rename saved")
        if "tempCorr" in corr:
            check(value(page, "tempCorr") == "-0.3", "gen 2 correction saved")
            r = gw.post("/sensors/gen2/edit", dict(data, tempCorr="1x"), ok=(400,))
            page = gw.get(f"/sensors/gen2?devId={dev}")
            check(r.status_code == 400 and value(page, "tempCorr") == "-0.3" and value(page, "name") == name + "-k",
                  "gen 2 correction '1x' refused with 400, nothing changed")
        # Forward to URL (2.0.0): saved, kept when not posted, invalid refused
        test_url = "http://192.0.2.1/x?t=*TEMP*&h=*HUM*"
        gw.post("/sensors/gen2/edit", dict(data, customUrl=test_url))
        page = gw.get(f"/sensors/gen2?devId={dev}")
        check(value(page, "customUrl") == test_url, "gen 2 forward URL saved")
        gw.post("/sensors/gen2/edit", data)
        page = gw.get(f"/sensors/gen2?devId={dev}")
        check(value(page, "customUrl") == test_url, "gen 2 forward URL kept when not posted")
        r = gw.post("/sensors/gen2/edit", dict(data, customUrl="ftp://x"), ok=(400,))
        page = gw.get(f"/sensors/gen2?devId={dev}")
        check(r.status_code == 400 and value(page, "customUrl") == test_url, "gen 2 forward URL ftp:// refused with 400")
        # Simple: name + interval + corrections
        gw.mode("simple")
        simple = gw.get(f"/sensors/gen2?devId={dev}")
        check("devNonce" not in simple and "Request status" not in simple, "gen 2 Simple page without radio internals")
        check(value(simple, "customUrl") == test_url, "gen 2 Simple page shows the forward URL")
        data = {"devId": dev, "name": name, "interval": value(simple, "interval"), "customUrl": url}
        for f in re.findall(r"name=(\w+Corr|altitude) ", simple):
            data[f] = before.get(f, value(simple, f))
        gw.post("/sensors/gen2/edit", data)
        gw.mode("advanced")
        page = gw.get(f"/sensors/gen2?devId={dev}")
        check(value(page, "name") == name and all(value(page, f) == before[f] for f in corr) and
              (value(page, "customUrl") or "") == url, "gen 2 name, forward URL and corrections restored")
    finally:
        data = {"devId": dev, "name": name, "customUrl": url}
        for f in corr:
            data[f] = before[f]
        gw.post("/sensors/gen2/edit", data)


# ----------------------------------------------------------- mode, /api

def test_misc(gw):
    print("Interface mode, /api")
    m = gw.mode()
    for bad in ({"mode": "expert"}, {}):
        r = gw.post("/system/mode", bad, ok=(400,))
        check(r.status_code == 400 and gw.mode() == m, f"POST /system/mode {bad}: 400, mode unchanged")
    ids = re.findall(r"/sensors/gen2\?devId=([0-9A-F]{8})", gw.get("/sensors"))
    if ids:
        one = requests.get(gw.base + f"/api?format=json&sensor={ids[0]}", timeout=10).json()["sensors"]
        check(len(one) == 1 and one[0]["serialNumber"] == ids[0], "/api?sensor=<DevID>: exactly that sensor")
        none = requests.get(gw.base + f"/api?format=json&sensor={ids[0]}&gen=1", timeout=10).json()["sensors"]
        check(none == [], "/api?sensor=<DevID>&gen=1: no row")


# ------------------------------------------------------- automatic updates

def auto_read(gw):
    p = gw.get("/firmware")
    return checked(p, "gw"), checked(p, "sn"), value(p, "ws"), value(p, "we")


def test_auto(gw):
    print("Automatic updates card (Firmware page)")
    start = auto_read(gw)
    try:
        gw.post("/firmware/auto", {"f": "1", "gw": "1", "ws": "23", "we": "1"})
        check(auto_read(gw) == (True, False, "23", "1"), "switches + window across midnight saved")
        # Only the switches (no window fields): the window is kept
        gw.post("/firmware/auto", {"f": "1", "sn": "1"})
        check(auto_read(gw) == (False, True, "23", "1"), "window kept when not posted")
        # Only the window (no f): the switches are kept
        gw.post("/firmware/auto", {"ws": "3", "we": "6"})
        check(auto_read(gw) == (False, True, "3", "6"), "switches kept without f")
        for bad in ({"ws": "4", "we": "4"}, {"ws": "24"}, {"we": "x"}, {"ws": "-1"}, {"ws": "007"}):
            r = gw.post("/firmware/auto", bad, ok=(400,))
            check(r.status_code == 400 and auto_read(gw) == (False, True, "3", "6"), f"refused {bad}, nothing changed")
    finally:
        d = {"f": "1", "ws": start[2] or "2", "we": start[3] or "5"}
        if start[0]:
            d["gw"] = "1"
        if start[1]:
            d["sn"] = "1"
        gw.post("/firmware/auto", d)
        check(auto_read(gw) == start, f"automatic updates restored {start}")


# --------------------------------------------------------------- network

def net(gw):
    p = gw.get("/network")
    return {"ssid": value(p, "ssid"), "wpw": placeholder(p, "password"), "wifiOn": checked(p, "wifiEnabled"),
            "ip": value(p, "ip"), "mask": value(p, "mask"), "gateway": value(p, "gateway"),
            "eth": selected(p, "ethMode"), "lteOn": checked(p, "lteEnabled"), "apn": value(p, "apn"),
            "pin": placeholder(p, "pin"), "page": p}


def wifi_connected(gw):
    return "<td>WiFi</td><td>connected" in gw.get("/network")


def test_network(gw, pro):
    print("Network (each save restarts the gateway)")
    gw.mode("simple")
    n0 = net(gw)
    ssid = n0["ssid"]
    if pro:
        # WiFi off: SSID / password hidden, not posted
        gw.post("/network", {"section": "wifi"})
        gw.wait_restart()
        n = net(gw)
        check(not n["wifiOn"] and n["ssid"] == ssid and n["wpw"] == "(unchanged)",
              "WiFi off: SSID and password kept")
        # WiFi on again: the browser posts the SSID and an empty password
        gw.post("/network", {"section": "wifi", "wifiEnabled": "1", "ssid": ssid, "password": ""})
        gw.wait_restart()
        time.sleep(20)
        check(net(gw)["wifiOn"] and wifi_connected(gw), "WiFi on again with the kept password: connected")
        # Ethernet DHCP: the static fields are hidden, not posted
        gw.post("/network", {"section": "ethernet", "ethMode": "dhcp"})
        gw.wait_restart()
        n = net(gw)
        check(n["eth"] == "dhcp" and (n["ip"], n["mask"], n["gateway"]) == (n0["ip"], n0["mask"], n0["gateway"]),
              f"Ethernet DHCP save: static addresses kept ({n['ip']}, {n['mask']}, {n['gateway']})")
        # LTE: an APN, then LTE off (APN / PIN hidden), then back as before
        if n0["lteOn"]:
            gw.post("/network", {"section": "lte", "lteEnabled": "1", "apn": "keep.test", "pin": ""})
            gw.wait_restart()
            gw.post("/network", {"section": "lte"})
            gw.wait_restart()
            n = net(gw)
            check(not n["lteOn"] and n["apn"] == "keep.test" and n["pin"] == n0["pin"], "LTE off: APN and PIN kept")
            gw.post("/network", {"section": "lte", "lteEnabled": "1", "apn": n0["apn"] or "", "pin": ""})
            gw.wait_restart()
            n = net(gw)
            check(n["lteOn"] and n["apn"] == n0["apn"] and n["pin"] == n0["pin"], "LTE restored")
        else:
            print("  (LTE is off on this gateway - LTE part skipped)")
    else:
        # Lite: WiFi is the only uplink - the same SSID with an empty
        # password must keep the stored one, or the gateway drops off
        gw.post("/network", {"section": "wifi", "ssid": ssid, "password": ""})
        gw.wait_restart()
        n = net(gw)
        check(n["ssid"] == ssid and n["wpw"] == "(unchanged)", "Lite WiFi save: SSID and password kept (reconnected)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True)
    ap.add_argument("--admin-password", required=True)
    ap.add_argument("--no-network", action="store_true", help="skip the network part (restarts)")
    a = ap.parse_args()
    gw = Gw(a.url, a.admin_password)
    try:
        gw.login()
        start = gw.mode()
        pro = "Use LTE" in gw.get("/network") or "Ethernet</h2>" in gw.get("/network")
        print(f"Gateway {a.url} ({'Pro' if pro else 'Lite'}), interface mode {start}")
        try:
            test_mqtt(gw)
            test_sensors(gw)
            test_misc(gw)
            test_auto(gw)
            if not a.no_network:
                test_network(gw, pro)
        finally:
            gw.mode(start)
            print(f"Interface mode restored: {gw.mode()}")
    except (RuntimeError, requests.exceptions.RequestException) as e:
        print(f"ERROR: {e}")
        return 2
    print(f"\n{'ALL PASSED' if not FAILURES else str(len(FAILURES)) + ' FAILED'}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
