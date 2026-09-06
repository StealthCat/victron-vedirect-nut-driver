#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cc -std=c99 -Wall -Wextra -Werror \
   -I"$HERE/src" \
   "$HERE/tests/test-parser.c" "$HERE/src/vedirect-parser.c" \
   -o "$HERE/tests/test-parser"
"$HERE/tests/test-parser"
