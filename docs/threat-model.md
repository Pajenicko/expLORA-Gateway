# Threat model - firmware updates (gateway 2.0.0)

Scope: how gateway firmware and sensor images reach a gateway and its
generation 2 sensors (signed files, the package manifest, update mode,
automatic updates, rollback). Formats and rules: `docs/protocol-v2.md`
§15; the release procedure: `docs/release.md`. The radio protocol's own
security (keys, MIC, replay counters) is in `docs/protocol-v2.md` §2-§5 and
§14.9.

## 1. Assets

- **Code running on the gateway** (gateway firmware) and **on the sensors**
  (sensor images, transferred by the gateway over LoRa).
- **The release private key** (ECDSA P-256; owner only, release machine +
  offline backup).
- **The update host** (`https://pajenicko.cz/webflash/`): `explora/v2/`
  (manifest + signed files) and the 1.x `explora-gw-<board>-autoupdate.json`.
- **Device state that guards the update path**: the replay floor `seq`,
  the install / strike records (`/update/state.json`), the stored sensor
  images and their "verified" metadata (`/ota/hw<HHHH>.json`), the admin
  password and sessions.
- **Availability**: a unit that keeps receiving sensors, and that can be
  updated again later (no bricking, no unbounded retry loops).

## 2. Attackers

1. **Network attacker (MITM)** between the gateway and the update host:
   can drop, delay, replay or alter traffic; cannot break TLS or forge
   ECDSA.
2. **Compromised web host**: can serve any file under the update URLs
   (old manifests, other binaries, a 1.x json pointing anywhere), but does
   not have the private key.
3. **Stolen or lost gateway / physical access**: UART/USB, flash read and
   write, power cuts at any moment.
4. **Malicious or careless admin** (logged in to the web UI).
5. **Compromised release machine** (or a leaked private key).
6. A compromised **LoRa neighbour** is covered by the radio protocol, not
   here.

## 3. What protects what

**Signature (ECDSA P-256 over the signature block, §15.2)**: a file is
"verified" only if a key compiled into the firmware signed its kind, board
/ HW type, version, length and SHA-256. A modified, truncated, re-labelled
(Lite image for the Pro, HW3 image for HW4) or re-versioned file is
refused; a cut download is refused before its tail is classified. The
manifest is signed over its exact bytes (§15.4); there is no unsigned
manifest. → Attackers 1 and 2 cannot make a gateway install code that the
owner did not sign.

**seq (monotonic, persisted)**: a gateway refuses a manifest older than
the newest it accepted. → An attacker cannot roll a unit back to an
earlier *offer* once it has seen a newer one. Automatic updates never
install a version older than the running one in any case (the version is
inside the signature), so an old manifest can at most withhold updates.

**HTTPS (built-in CA bundle, https-only redirects, strict length)**:
confidentiality and integrity of the transport, and some protection of
availability against attacker 1. It does not replace the signature: a
compromised host has a valid certificate.

**Rollback / validation**: a new gateway firmware must run 120 s with the
web server up and the internal liveness checks healthy, otherwise the
bootloader returns to the previous firmware; automatic installs count
strikes and give a version up after three (§15.6). → A broken (or
deliberately crashing) release cannot loop or brick units that update
automatically.

**Admin confirmation / "Signed images only"**: unsigned images, images
signed with an unknown key and downgrades need an explicit confirmation
by the session that wrote them (10 minutes, bound to the image hash); with
"Signed images only" unsigned images are refused. Automatic paths take
signed (Verified) files only, whatever that setting says.

### What it does NOT protect

- **The admin can install anything** (by design, owner decision): an
  unsigned image after the confirmation, any older signed version
  (downgrade with confirmation, also with "Signed images only" on - there
  is no anti-rollback counter), and any sensor image. A stolen admin
  password is therefore code execution on the gateway and on its sensors.
- **Physical access is game over**: no secure boot, no flash encryption.
  UART/USB can flash anything, read the sensor keys and the admin
  password hash, edit `state.json`, the stored images or their "verified"
  metadata. The signatures protect only the update path.
- **The sensors trust the gateway**: the sensor and its bootloader check
  CRC, HW type and trailer only; the signature is checked by the gateway.
  A compromised gateway (or its admin) can push any image to its sensors.
- **Stored metadata is trusted**: a stored sensor image's "verified" flag
  (`hw<HHHH>.json`) is not re-verified later (only its CRC when a job
  starts), nor against a key table that changed since. Same trust model as
  the rest of the file system (attacker 3 is out of scope anyway).
- **Filesystem loss resets seq** to 0 (formatted / lost filesystem, a
  rollback or downgrade to a firmware that drops the field, a state file
  that cannot be read and holds no complete number). Bounded: nothing older
  than the running firmware is installed from a manifest.
- **Freeze / withholding**: manifests do not expire (owner decision). A
  compromised host or a MITM can keep serving an old manifest (same or
  higher seq it has seen) or nothing at all; a unit then simply does not
  update. The same seq is accepted again (the same manifest re-checked).
- **The 1.x path**: 1.x units read `explora-gw-<board>-autoupdate.json`
  over HTTPS only, without signatures. A compromised host can make a 1.x
  unit install any image via "Check for updates" (the admin still clicks
  "Update"). This is the 1.x design and ends once the unit runs 2.x.
- **A compromised release machine or key** (attacker 5) can sign anything;
  combined with control of the host it can update every unit that has
  automatic updates on. There is no revocation list: a leaked key is
  retired only by a firmware that drops it (slot 1 exists for rotation,
  `docs/release.md` §8).
- **Availability during updates**: every update session pauses LoRa
  reception and MQTT (up to 15 minutes for a gateway download, about 2
  minutes for a sensor image); uplinks in that time are lost.

## 4. Attacker by attacker

| Attacker | Can | Cannot |
|---|---|---|
| Network MITM | block or delay checks and downloads (no update); make a download fail (no strike for network errors); cause a 404 / 410 answer - only with a TLS break, so not in practice | install unsigned or foreign code; replay an older manifest after a newer one was seen; downgrade |
| Compromised host | withhold updates; serve an old manifest the unit has not seen beyond; serve wrong files (refused, may cost a version three strikes and be given up until another version is offered); a 404 / 410 / non-image body (same); control the 1.x autoupdate json (1.x units only) | make a 2.x unit run code the owner did not sign |
| Stolen gateway | everything on that unit (flash, keys of its sensors, admin hash) | affect other units (no shared secret on the gateway except the public release keys) |
| Admin | install any image (confirmation), downgrade, switch "Signed images only" off, push any sensor image | (by design nothing prevents this - the admin owns the unit) |
| Release machine / key | sign any firmware; with host control, push it to auto-updating units | reach units whose firmware no longer trusts that key |

## 5. Residual risks (accepted or documented)

Trust and keys
- No secure boot / flash encryption; physical access bypasses everything.
- Admin can install unsigned images and any older signed version; no
  anti-rollback counter.
- Sensors rely on the gateway's verification only.
- No key revocation list; rotation only through a firmware release (two
  key slots). The private key exists on one machine plus an offline backup.
- Stored "verified" sensor image metadata is not re-verified later.
- With no admin password set the confirmation's session binding falls back
  to a constant (the firmware forces setting a password first).

Manifest and replay
- No expiry: freeze / withholding attacks possible; equal seq accepted.
- seq floor resets to 0 when the filesystem or the state file is lost, after
  a rollback to a firmware without the field, or while a save keeps failing.
- An older firmware drops unknown `state.json` fields (the S3 contract):
  after a rollback the automatic-update record and seq written by the newer
  firmware are lost.
- An install queued from manifest N still runs after a later check accepted
  N+1 (it is signed and newer than the running firmware).
- A manifest URL behind a redirect must start its second hop within ~7 s
  (20 s budget, 15 s hop reserve); over LTE a redirecting manifest URL can
  fail with "too slow". The manifest tool is stricter than the gateway in a
  few JSON details (safe direction).

Gateway images, activation, validation
- Validation does not require internet connectivity (owner decision): a
  firmware that breaks the uplink but runs still validates.
- Any reset inside the 120 s validation window (power cut, heap restart,
  restore restart) rolls a good firmware back and counts a strike; three
  such nights give the version up until another one is offered.
- markValid failing 5 times leaves the image unvalidated: the next restart
  rolls back a working firmware (fail-safe, logged).
- A new firmware refuses installs and checks during its first 120 s.
- A failed `state.json` save at activation still activates; the install
  record may then be missing.
- (S3 leftover d) An active VALID otadata entry naming a corrupt slot cannot
  be invalidated by `esp_ota_begin`; an image later written into that slot
  and left waiting for its confirmation could then boot without it. Needs
  a flash error in otadata; exotic. Likewise a failed invalidation of the
  inactive otadata entry can leave an abandoned image with a stale VALID
  entry that is not erased at boot (fail-safe for the rollback target).
- Writing only the application into the running slot with esptool keeps
  the install record, which then describes the wrong image.
- A manually uploaded signed file with bytes appended or its block cut off
  counts as unsigned (needs the confirmation), not as refused; a sensor file
  cut exactly at the image end is stored as unsigned.
- The flash write permit is held during image verification at validation
  and activation (hundreds of ms): timed radio responses are skipped then.
- A firmware whose version text is not maj.min.pat (development builds)
  sends every verified image through the confirmation, gets no automatic
  install and reports a false rollback for its own automatic install.

Update mode
- LoRa paused and MQTT held during every update session; heavy pages 503;
  on the Pro the web UI is practically unreachable during the TLS part.
- The restore watch restarts the gateway when an interface it switched
  off is not back within 5 minutes (also a slow LTE re-attach, or an
  unrelated uplink loss in that time); a deferred restore can last while a
  sensor job swaps or confirms.
- The DNS lookup of a redirect host is not bounded by the hop reserve and
  can extend the radio pause by the lwIP retry time.

Automatic updates
- Download failures other than HTTP 404 / 410 and a crash during the
  download (every other 4xx such as a CDN bot challenge 403 or a 401, 5xx,
  timeouts, cut transfers) never strike: a permanently broken entry is
  retried every night (at most ~6 downloads per night), and a host or CDN
  that keeps refusing with 403 costs data volume but never gives a version
  up.
- A host that answers 200 with a non-image body, or 404 / 410 (a file
  missing or misnamed for three nights), strikes the version: after three
  nights every automatically updating unit gives it up until another
  version number is offered (an owner / host fault, bounded by the signed
  manifest; re-uploading the same version does not help - docs/release.md).
- A power loss during an automatic download is counted like a crash (one
  strike for that night).
- RAM-only: the download failure count, the per-sensor holds (24 h after
  an admin abort, 30 min after a refused start).
- Without an accepted manifest, a rolled-back version holds automatic
  sensor jobs back for at most 3 nights.
- Nothing automatic runs without a recent SNTP time.
- A window lying wholly inside a skipped DST hour does not run that night
  if the gateway restarts after the clock change.
- A failed check drops the previous offers.

Release
- Bench gateways keep seq floors from bench manifests; the first
  production seq must be above them (`docs/release.md`).
