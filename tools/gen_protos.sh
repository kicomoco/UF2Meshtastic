#!/usr/bin/env bash
# Regenerates nanopb C sources for the Meshtastic protobufs into src/proto.
# Requires: .deps/protobufs (meshtastic/protobufs), .deps/nanopb and a python
# venv in .deps/venv with `protobuf` and `grpcio-tools` installed (see setup.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
D="$ROOT/.deps"
OUT="$ROOT/src/proto"
rm -rf "$OUT" && mkdir -p "$OUT"
"$D/venv/bin/python" "$D/nanopb/generator/nanopb_generator.py" -q -D "$OUT" \
    -I "$D/protobufs" -I "$D/nanopb/generator/proto" "$D"/protobufs/meshtastic/*.proto
# Device-only / C++-only definitions are not needed by a client.
rm -f "$OUT"/meshtastic/{deviceonly,deviceonly_legacy,clientonly}.pb.{c,h}
echo "Generated protobufs into $OUT"
