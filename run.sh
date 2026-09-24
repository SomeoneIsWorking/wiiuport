#!/usr/bin/env sh
# Launch the runtime. Everything else lives in Python; keep this a shim.
set -eu
cd "$(dirname "$0")"
exec uv run --frozen python bootstrap.py "$@"
