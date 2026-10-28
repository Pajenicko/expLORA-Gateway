# Release procedure (gateway 2.x, sensor images)

For the release manager. Format and rules: `docs/protocol-v2.md` §15;
threats: `docs/threat-model.md`. Nothing here is automated on purpose:
the private key never leaves the signing machine, and the tools contain no
upload code.

Placeholders used below:

- `<KEY>` - the private release key (PEM). The tools take it only as a
  path (`--key`, or `$EXPLORA_SIGN_KEY`); they refuse a key readable by
  group or others and never print it. Never copy it into a repository,
  a build folder, CI or a chat.
- `<PUB>` - the matching public key (`*.pub.pem`, written by
  `explora_keygen.py`); its key id must be the one in
  `src/Security/ReleaseKeys.h` slot 0 (`76091636` for 2.0.0).
- `<VER>` - the gateway version (`src/version.h`), `<SVER>` - the sensor
  version (`custom_fw_version` in the sensor's `platformio.ini`), `<HHHH>` -
  the sensor HW type in 4 hex digits (`0003` = lorasen v3 core).
- `<BASE>` = `https://pajenicko.cz/webflash/explora/v2` (the folder of
  `packages.json`; every package URL is `<BASE>/<file name>`).

Requirements: Python 3 with `cryptography`, PlatformIO.

## 1. Build

From the release commit, clean trees (`git status` empty, no bench edits;
`ReleaseKeys.h` as committed):

```
cd expLORA-Gateway
pio run -e pro          # .pio/build/pro/firmware.bin
pio run -e lite         # .pio/build/lite/firmware.bin
pio test -e native      # all pass
python3 tools/release/test_release_tools.py

cd ../expLORA-Sensors-gen2
pio run -e e77 -t clean && pio run -e e77   # .pio/build/e77/firmware.bin (HW 0x0003)
```

The gateway version is `FIRMWARE_VERSION` in `src/version.h` (plain
maj.min.pat, no suffix: the sign tool refuses a pre-release without
`--force`, and a gateway cannot compare such a version). The sensor build
writes the image trailer (`EXLT`, HW type, version, CRC) itself. Only the
normal environments: never sign a bench build (`e77_bench`, bench CA,
bench keys).

Record the commit hashes of both repositories and the SHA-256 of the
three unsigned files.

## 2. Sign

```
mkdir -p dist
python3 tools/release/explora_sign.py sign --kind gateway --board pro  --version <VER> --key <KEY> \
    .pio/build/pro/firmware.bin  dist/gw-pro-<VER>.bin
python3 tools/release/explora_sign.py sign --kind gateway --board lite --version <VER> --key <KEY> \
    .pio/build/lite/firmware.bin dist/gw-lite-<VER>.bin
python3 tools/release/explora_sign.py sign --kind sensor --key <KEY> \
    ../expLORA-Sensors-gen2/.pio/build/e77/firmware.bin dist/hw<HHHH>-<SVER>.bin
for f in dist/*.bin; do python3 tools/release/explora_sign.py verify --pub <PUB> "$f" || echo "FAILED $f"; done
```

File names are fixed: `gw-pro-<ver>.bin`, `gw-lite-<ver>.bin`,
`hw<HHHH>-<ver>.bin` (the manifest tool derives the URLs from them; they
are never reused for other content - a new build gets a new version).
The sign tool takes board / HW type and version from the file and only
cross-checks `--board` / `--version`; it verifies its output with the
public key before it reports success.

## 3. Manifest

```
python3 tools/release/explora_manifest.py build --base <BASE> --seq <SEQ> --key <KEY> \
    -o dist/packages.json dist/gw-pro-<VER>.bin dist/gw-lite-<VER>.bin dist/hw<HHHH>-<SVER>.bin
python3 tools/release/explora_manifest.py verify --pub <PUB> --min-seq <SEQ> dist/packages.json
```

**seq rules**: every published manifest has a seq above the previous one
(`--seq auto --prev <BASE>/packages.json` reads the published one, verifies
it and adds 1). A gateway refuses a seq below the highest it accepted, so
a lower one is never usable again. **The first production manifest needs
a seq above the floor of the bench gateways**: they accepted bench-key
manifests (73 on the Pro, 72 on the Lite at the end of the 2.0.0
preparation; the floor stays on the device, customer units start at 0, so
a higher first seq costs nothing). Right before the release read the
stored seq from both bench units again - logged in, `GET /diag/heap` ->
`update.manifestSeq` (or the last check line on the Firmware page,
"ok (seq N)") - and use a seq above both that leaves room, e.g. 100.

`build` refuses anything a gateway would skip or refuse (sizes, URLs,
versions, duplicate boards / HW types, unsigned files); `--allow-skip` is
only for an entry meant for later firmware. Keep `dist/packages.json`
under 4096 bytes (checked).

## 4. Upload (manual, in this order)

1. **Binaries first**: upload `gw-pro-<VER>.bin`, `gw-lite-<VER>.bin`,
   `hw<HHHH>-<SVER>.bin` into the `explora/v2/` folder. Never overwrite or
   delete the existing 1.x files (`explora-gw-lite/`, `explora-gw-pro/`,
   the 1.x manifests).
2. **Verify each binary over HTTPS** (size and SHA-256 must equal the local
   file and the manifest) - **only after its upload has completed**. The
   site is behind Cloudflare, which caches `.bin` responses: a GET of a
   file that is not (completely) there yet caches the 404 or the partial
   answer at that edge for minutes. Use a GET (never HEAD) with a
   cache-busting query, so the check reaches the origin:
   ```
   for f in dist/*.bin; do
     n=$(basename $f); curl -fsS -o /tmp/chk.bin "<BASE>/$n?v=$(date +%s)" &&
     echo "$n $(stat -c %s /tmp/chk.bin) $(sha256sum < /tmp/chk.bin | cut -c1-64) local $(stat -c %s $f) $(sha256sum < $f | cut -c1-64)"
   done
   ```
   Then once more without the query (what the gateways get from the
   cache) - same size and SHA-256. The server must answer 200 with a
   correct `Content-Length` and nothing after it (a gateway refuses a body
   longer or shorter than announced). Never verify a file name before it
   is uploaded.
3. **Then the manifest**: upload `packages.json` under a temporary name
   (e.g. `packages.json.new`), check it over HTTPS
   (`explora_manifest.py verify --pub <PUB> --min-seq <SEQ> <BASE>/packages.json.new`),
   then rename it to `packages.json` on the server (an atomic rename: a
   gateway never sees half a file).
4. **Post-upload verification**:
   ```
   python3 tools/release/explora_manifest.py verify --pub <PUB> --min-seq <SEQ> <BASE>/packages.json
   curl -fsS -o /tmp/chk.json "<BASE>/packages.json?v=$(date +%s)" && cmp /tmp/chk.json dist/packages.json
   curl -fsS -o /tmp/chk.json "<BASE>/packages.json" && cmp /tmp/chk.json dist/packages.json
   ```
   (GET, not `curl -I`: Cloudflare may answer a HEAD from its cache while
   the origin already has the new file.)
   On a gateway running the previous 2.x version: Firmware page -> **Check
   now** must answer "ok (seq <SEQ>): v<VER> available" (Advanced mode
   also lists the sensor images); on one running <VER>: "up to date". The
   log has one line per skipped entry - there must be none.

## 5. The 1.x path (Lite units in the field)

Units on 1.x read only `https://pajenicko.cz/webflash/explora-gw-lite-autoupdate.json`
("Check for updates": the browser compares the versions and installs the
URL over HTTPS with the built-in CA bundle). Its exact format today
(read 2026-10-28):

```
{
    "version": "1.2.0",
    "url": "https://pajenicko.cz/webflash/explora-gw-lite/firmware_1.2.0.bin"
}
```

(JSON, 4-space indent, served as `application/json`; 1.x reads at most
2 kB of it and needs only `version` and `url`.) To offer 2.0.0:

1. Only after the 2.0.0 files are published and verified (section 4) and
   the bench migration test 1.2.0 -> 2.0.0 passed on a Lite.
2. **Backup**: copy the current file to the archive folder on the server
   (`_archiv/webflash-<date>/explora-gw-lite-autoupdate.json`) and keep a
   local copy.
3. Replace its content (same formatting) with
   ```
   {
       "version": "2.0.0",
       "url": "https://pajenicko.cz/webflash/explora/v2/gw-lite-2.0.0.bin"
   }
   ```
   The signed file works for 1.x (it ignores the 128-byte block after the
   image). Write it under a temporary name and rename it, as for the
   manifest.
4. Verify over HTTPS (`curl -fsS ...autoupdate.json`), then on a Lite with
   1.2.0: Firmware -> "Check for updates" must offer 2.0.0 and install it;
   after the restart check the migration points of section 7.
5. Only the Lite file is changed (owner decision 2026-10-27; the Pro file
   stays at 1.2.0 unless the owner decides otherwise). The 1.x binaries
   are never overwritten or deleted.

## 6. If a release is bad

Automatic updates never install an older version, so publishing a higher
seq that names the previous version does **not** bring units back. What
to do instead:

- **Stop the spread at once**: publish a new manifest (seq + 1) without the
  bad entry (or with the previous good entries only). Units that have not
  installed it yet see no offer. For the 1.x path, restore the backup of
  `explora-gw-lite-autoupdate.json` (section 5).
- **Firmware that crashes or hangs before its validation** (120 s with the
  web server up) rolls back by itself; the automatic install gives a
  version up after three strikes (three nights). Nothing to do on the units.
- **A file missing for three nights**: a package URL that answers 404 or
  410 (file not uploaded, misnamed, deleted) is a strike on every unit
  with automatic updates, one per night; after three nights the whole
  fleet gives that version up, and re-uploading the file under the same
  version does **not** help - only a new version number (`<VER+1>` /
  `<SVER+1>`, new files, higher seq) is installed automatically again
  (manual **Install** still works). The upload order of section 4
  (binaries first and verified, the manifest last) avoids it; never
  rename or delete a published binary while a manifest names it. Other
  answers (403 from a CDN challenge, 401, 5xx, timeouts) are no strike.
- **Firmware that validates but is wrong**: fix forward - release
  `<VER+1>` (new file names, higher seq). Units with automatic updates
  install it in their next window; others get it with **Check now** /
  **Install**.
- **Manual downgrade** where needed: the admin uploads the previous signed
  file (or its URL) on the Firmware page; a verified older version asks
  for the confirmation (downgrade) and is then installed. Settings and
  `sensors.json` are kept; a firmware older than 2.0.0 drops the 2.x
  fields it does not know (`docs/protocol-v2.md` §15.7).
- **Bad sensor image**: publish a manifest without it (or with a fixed
  `<SVER+1>`). A sensor that does not confirm the new image reverts by
  itself (§10.3) and never gets that version automatically again; the
  automatic job of a version stops after two failures per sensor. An
  older sensor image can be installed by the admin (a downgrade needs the
  confirmation on the gen 2 page).
- Never re-sign a different build under a version that was published, and
  never lower the seq.

## 7. Migration checks (1.x -> 2.0.0)

After a 1.2.0 unit installed 2.0.0 (bench before the release, then one
real unit through the 1.x path): the gateway validates after 2 minutes
(Firmware page: "validated"), WiFi / network settings, admin password,
time zone, MQTT settings, `sensors.json` (all generation 1 sensors with
their keys, names, forward URLs and corrections) are unchanged, the
interface starts in Simple mode, automatic updates are off, and the
first **Check now** reads the 2.x manifest.

## 8. Key rotation / loss

The firmware carries two key slots (`ReleaseKeys.h`; 2.0.0: slot 0 =
`76091636`, slot 1 empty). To rotate: generate the new key
(`explora_keygen.py`, owner only), release a firmware with the new public
key in slot 1 (signed with the current key), wait until the fleet runs it,
then sign with the new key and move it to slot 0 in a later release.

- **Lost key** (no backup): units can no longer be updated automatically
  or by a verified manual install; an admin can still install an unsigned
  file with the confirmation (or with "Signed images only" off). A release
  with a new key must then be installed by hand on every unit. Keep the
  offline backup.
- **Compromised key**: whoever holds it can sign firmware every unit
  accepts as verified - but to reach units automatically they also need
  to serve a manifest from `<BASE>` (the web host) with a seq above the
  units' floor. Immediately: check the web host and the published files;
  release a firmware that carries a new key and removes the old one
  (signed with the old key, the only one the units know), publish it with
  a higher seq, and sign everything after it with the new key only. Units
  that do not update stay exposed (`docs/threat-model.md`).
