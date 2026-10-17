#!/bin/sh
# Keep lib/explora-protocol (and its native tests) identical in
# expLORA-Gateway (source of truth) and expLORA-Sensors-gen2.
#
#   scripts/sync_protocol.sh            copy gateway -> sensor
#   scripts/sync_protocol.sh --check    only compare, exit 1 when they differ
#
# Works from either repository; the other checkout is expected next to it
# (override with GATEWAY_DIR / SENSOR_DIR).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
parent=$(dirname "$here")
GATEWAY_DIR=${GATEWAY_DIR:-$parent/expLORA-Gateway}
SENSOR_DIR=${SENSOR_DIR:-$parent/expLORA-Sensors-gen2}
paths="lib/explora-protocol test/test_protocol_v2 scripts/sync_protocol.sh"

for p in $paths; do
    [ -e "$GATEWAY_DIR/$p" ] || { echo "missing $GATEWAY_DIR/$p" >&2; exit 2; }
done

if [ "$1" = "--check" ]; then
    status=0
    for p in $paths; do
        diff -r "$GATEWAY_DIR/$p" "$SENSOR_DIR/$p" >/dev/null 2>&1 || { echo "differs: $p"; status=1; }
    done
    [ $status -eq 0 ] && echo "explora-protocol identical"
    exit $status
fi

for p in $paths; do
    mkdir -p "$SENSOR_DIR/$(dirname "$p")"
    rm -rf "${SENSOR_DIR:?}/$p"
    cp -R "$GATEWAY_DIR/$p" "$SENSOR_DIR/$p"
done
echo "copied to $SENSOR_DIR"
