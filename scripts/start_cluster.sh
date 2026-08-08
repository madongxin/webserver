#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GAMEMESH_FORMAL="${GAMEMESH_FORMAL:-1}"
export GAMEMESH_ADVERTISE_HOST="${GAMEMESH_ADVERTISE_HOST:-127.0.0.1}"
export GAMEMESH_HTTP_BIND="${GAMEMESH_HTTP_BIND:-127.0.0.1}"
export GAMEMESH_ENABLE_ADMIN="${GAMEMESH_ENABLE_ADMIN:-0}"
exec "$ROOT/scripts/start_formal.sh" "$@"
