#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
ROOT="$(cd "$ROOT" && pwd)"
DRIVERS="$ROOT/drivers"
MAKEFILE="$DRIVERS/Makefile.am"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -f "$MAKEFILE" ]]; then
    echo "ERROR: $ROOT does not look like a NUT source tree (drivers/Makefile.am missing)." >&2
    exit 1
fi

cp "$HERE/src/victron-vedirect.c" "$DRIVERS/victron-vedirect.c"
cp "$HERE/src/vedirect-parser.c" "$DRIVERS/vedirect-parser.c"
cp "$HERE/src/vedirect-parser.h" "$DRIVERS/vedirect-parser.h"

python3 - "$MAKEFILE" <<'PY'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()

if "victron_vedirect_SOURCES" not in s:
    anchor = "ve_direct_SOURCES = ve-direct.c\nve_direct_LDADD = $(LDADD_DRIVERS_SERIAL) -lm"
    if anchor not in s:
        raise SystemExit("ERROR: Could not find the expected ve-direct build stanza. This installer targets NUT 2.8.5/compatible 2.8.x trees.")
    s = s.replace(
        anchor,
        anchor + "\n\nvictron_vedirect_SOURCES = victron-vedirect.c vedirect-parser.c\n"
                 "victron_vedirect_LDADD = $(LDADD_DRIVERS_SERIAL) -lm",
        1,
    )

if " victron-vedirect " not in s and "victron-vedirect meanwell_ntu" not in s:
    anchor = "bicker_ser ve-direct meanwell_ntu"
    if anchor not in s:
        raise SystemExit("ERROR: Could not find the expected SERIAL_DRIVERLIST entry. This installer targets NUT 2.8.5/compatible 2.8.x trees.")
    s = s.replace(anchor, "bicker_ser ve-direct victron-vedirect meanwell_ntu", 1)

p.write_text(s)
PY

echo "Installed driver sources into: $DRIVERS"
echo
echo "Next steps:"
echo "  cd '$ROOT'"
echo "  ./autogen.sh"
echo "  ./configure --with-drivers=victron-vedirect"
echo "  make -j\"\$(nproc)\""
echo "  sudo make install"
echo
echo "Then configure ups.conf using this bundle's ups.conf.example."
