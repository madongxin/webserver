#!/usr/bin/env bash
# Multi-role image dispatcher: role name or absolute binary path.
# Avoids ENTRYPOINT=gateway + command=[session,...] becoming "gateway session ...".
set -euo pipefail

ROOT="${GAMEMESH_BIN_DIR:-/opt/gamemesh}"

if [[ $# -eq 0 ]]; then
  echo "usage: gateway|session|gamelogic|world|gamedb [args...]" >&2
  echo "   or: /absolute/path/to/binary [args...]" >&2
  exit 2
fi

cmd="$1"
shift

case "$cmd" in
  gateway|session|gamelogic|world|gamedb)
    exec "${ROOT}/${cmd}" "$@"
    ;;
  /*)
    exec "$cmd" "$@"
    ;;
  *)
    if [[ -x "${ROOT}/${cmd}" ]]; then
      exec "${ROOT}/${cmd}" "$@"
    fi
    echo "ERROR: unknown role/binary: $cmd" >&2
    exit 2
    ;;
esac
