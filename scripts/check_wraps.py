"""
Fail the build when a linker wrap is configured but not linked

DriverConfig.cpp replaces driver settings through -Wl,--wrap=<symbol> (see
platformio.ini). A wrap silently does nothing when its __wrap_<symbol> is
missing (source not built for this board) or when no caller is redirected
(the caller and the callee sit in one prebuilt object, a renamed function):
--gc-sections drops an unreferenced __wrap_<symbol>, so it is in the
firmware only while the linker routes calls to it. After linking, every
configured wrap must show __wrap_<symbol> as a defined function and the
original symbol as still present.

The other way round, every __wrap_<symbol> that DriverConfig.cpp compiled
for this board (its object file, before --gc-sections) must have its
-Wl,--wrap flag: a flag lost from build_flags (or an env without any)
would otherwise pass silently with the stock driver settings.
"""

import os
import re
import subprocess

Import("env")  # noqa: F821


def configured_wraps(env):
    # Only this project's build_flags: the framework adds wraps of its own
    # (Matter, longjmp) that this firmware does not link
    wraps = []
    for flag in env.GetProjectOption("build_flags", []):
        for m in re.finditer(r"--wrap=([A-Za-z0-9_]+)", str(flag)):
            wraps.append(m.group(1))
    return sorted(set(wraps))


def nm_tool(env):
    cc = env.subst("$CC")
    base = os.path.basename(cc)
    if not base.endswith("gcc"):
        return None
    return os.path.join(os.path.dirname(cc), base[: -len("gcc")] + "nm")


def symbols(env, path):
    out = subprocess.run([nm_tool(env), path], capture_output=True, text=True, check=True, env=env["ENV"]).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            found[parts[2]] = parts[1]
    return found


def check_wraps(target, source, env):
    wraps = configured_wraps(env)
    elf = str(target[0])
    obj = os.path.join(env.subst("$BUILD_DIR"), "src", "Network", "DriverConfig.cpp.o")
    try:
        compiled = sorted(name[len("__wrap_"):] for name, kind in symbols(env, obj).items()
                          if name.startswith("__wrap_") and kind in ("T", "t"))
        out_symbols = symbols(env, elf)
    except Exception as e:  # tool or object missing / failed: say so, do not pass silently
        print("check_wraps.py: ERROR - cannot list the symbols of %s / %s: %s" % (obj, elf, e))
        return 1
    unflagged = [w for w in compiled if w not in wraps]
    if unflagged:
        print("check_wraps.py: ERROR - DriverConfig.cpp defines __wrap_ functions without their "
              "-Wl,--wrap flag in build_flags: %s" % ", ".join(unflagged))
        return 1
    if not wraps:
        print("check_wraps.py: ERROR - no linker wraps configured and none compiled - expected at least "
              "esp_wifi_init (DriverConfig.cpp)")
        return 1
    defined = out_symbols
    missing = [w for w in wraps if defined.get("__wrap_" + w) not in ("T", "t")]
    gone = [w for w in wraps if w not in defined]
    if missing or gone:
        if missing:
            print("check_wraps.py: ERROR - wrap configured but not linked (no caller redirected or "
                  "no __wrap_ function): %s" % ", ".join(missing))
        if gone:
            print("check_wraps.py: ERROR - wrapped symbol not in the firmware: %s" % ", ".join(gone))
        return 1
    print("check_wraps.py: linked wraps: %s" % ", ".join(wraps))
    return 0


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", env.VerboseAction(check_wraps, "Checking linker wraps"))  # noqa: F821
