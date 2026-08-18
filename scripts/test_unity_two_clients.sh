#!/usr/bin/env bash
# 真实 Unity 双客户端门禁（本轮 P0：Hello/登录/同图/互见/双向移动/Logout Leave）。
# 缺 LUNA_REPO、UNITY_CLIENT_BIN、live Gateway 或协议不一致 → BLOCKED/FAIL，不得 PASS。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

die() {
  echo "UNITY TWO CLIENTS BLOCKED: $*" >&2
  echo "unity_two_clients=BLOCKED" >&2
  exit 1
}

LUNA="${LUNA_REPO:-}"
BIN="${UNITY_CLIENT_BIN:-}"
if [[ "${1:-}" != "" && "${1}" != --* ]]; then
  LUNA="$1"
fi
if [[ "${2:-}" != "" ]]; then
  BIN="$2"
fi
[[ -n "$LUNA" ]] || die "LUNA_REPO unset"
[[ -d "$LUNA" ]] || die "LUNA_REPO is not a directory: $LUNA"
LUNA="$(cd "$LUNA" && pwd)"
if [[ -z "$BIN" ]]; then
  for cand in \
    "$LUNA/Builds/GameMeshClient/GameMeshClient.x86_64" \
    "$LUNA/Builds/GameMeshClient/GameMeshClient" \
    "$LUNA/Builds/GameMeshClient/GameMeshClient.exe"; do
    if [[ -x "$cand" || -f "$cand" ]]; then
      BIN="$cand"
      break
    fi
  done
fi
[[ -n "$BIN" ]] || die "UNITY_CLIENT_BIN unset and no Luna Builds/GameMeshClient binary"
[[ -x "$BIN" || -f "$BIN" ]] || die "UNITY_CLIENT_BIN missing: $BIN"
export LUNA_REPO="$LUNA"
export GAMEMESH_REQUIRE_LUNA_CONTRACT=1

"$ROOT/scripts/check_luna_protocol_contract.sh" "$LUNA" \
  || die "protocol contract failed"

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/unity-e2e}"
# shellcheck disable=SC1090
source "$ROOT/scripts/e2e_inventory.sh"
e2e_ensure_cluster "$ROOT" || die "cluster unhealthy"
# shellcheck disable=SC1090
[[ -f "$GAMEMESH_RUN_DIR/E2E_PORTS.env" ]] && source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
HOST="${E2E_HOST:-127.0.0.1}"
GW0="${E2E_GW0_GAME:?}"
GW1="${E2E_GW1_GAME:?}"

UNITY_SCRIPT=""
for cand in \
  "$LUNA/Tools/GameMesh/run_two_clients_e2e.sh" \
  "$LUNA/Tools/GameMesh/run_two_clients.sh" \
  "$LUNA/Tools/GameMesh/test_two_clients.sh" \
  "$LUNA/Tools/GameMesh/e2e_two_clients.sh"; do
  if [[ -f "$cand" ]]; then
    UNITY_SCRIPT="$cand"
    break
  fi
done
[[ -n "$UNITY_SCRIPT" ]] || die "missing Luna two-client script under Tools/GameMesh/"

STAMP="$(date +%s)"
OUT_DIR="${ROOT}/run/unity_two_clients/${STAMP}"
mkdir -p "$OUT_DIR"
{
  echo "server_commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "luna_commit=$(git -C "$LUNA" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "schema_sha256=$(sha256sum "$ROOT/proto/game.proto" | awk '{print $1}')"
  echo "host=$HOST gw0=$GW0 gw1=$GW1"
} >"$OUT_DIR/meta.txt"

chmod +x "$UNITY_SCRIPT" 2>/dev/null || true
set +e
if [[ "$(basename "$UNITY_SCRIPT")" == "run_two_clients_e2e.sh" ]]; then
  export GAMEMESH_E2E_GATEWAY=1
  export GAMEMESH_HOST="$HOST"
  export GAMEMESH_PORT="$GW0"
  export GAMEMESH_E2E_SCENARIO="${GAMEMESH_E2E_SCENARIO:-presence-move-logout}"
  "$UNITY_SCRIPT" "$BIN" >"$OUT_DIR/unity_runner.log" 2>&1
  rc=$?
  # Luna writes Logs/e2e-*/{a,b}/result.json under the client repo.
  latest="$(ls -1dt "$LUNA"/Logs/e2e-* 2>/dev/null | head -1 || true)"
  if [[ -n "$latest" ]]; then
    cp -a "$latest/." "$OUT_DIR/" 2>/dev/null || true
    if [[ -f "$latest/a/result.json" && -f "$latest/b/result.json" ]]; then
      python3 - "$latest/a/result.json" "$latest/b/result.json" "$OUT_DIR/result.json" <<'PY'
import json, sys
a = json.load(open(sys.argv[1]))
b = json.load(open(sys.argv[2]))
out = {
    "hello_ok": bool(a.get("hello_ok") and b.get("hello_ok")),
    "login_ok": bool(a.get("login_ok") and b.get("login_ok")),
    "same_instance": bool(a.get("map_instance_id_before_logout") and
                          a.get("map_instance_id_before_logout") == b.get("map_instance_id_before_logout")),
    "aoi_enter_ok": bool(a.get("peer_seen") and b.get("peer_seen")),
    "aoi_move_ok": bool(a.get("peer_move_seen") and b.get("peer_move_seen")),
    "logout_leave_ok": bool(a.get("logout_rsp_ok") and b.get("logout_rsp_ok") and b.get("peer_leave_seen")),
    "a": a,
    "b": b,
}
json.dump(out, open(sys.argv[3], "w"), indent=2)
print("merged", sys.argv[3])
PY
    fi
  fi
else
  "$UNITY_SCRIPT" \
    --server-repo "$ROOT" \
    --client-bin "$BIN" \
    --host "$HOST" \
    --gw0 "$GW0" \
    --gw1 "$GW1" \
    --out "$OUT_DIR" >"$OUT_DIR/unity_runner.log" 2>&1
  rc=$?
fi
set -e
cat "$OUT_DIR/unity_runner.log"

[[ "$rc" -eq 0 ]] || die "unity two-client script rc=$rc (see $OUT_DIR)"
[[ -f "$OUT_DIR/result.json" ]] || die "missing $OUT_DIR/result.json"
python3 - "$OUT_DIR/result.json" <<'PY' || exit 1
import json, sys
p = sys.argv[1]
r = json.load(open(p))
need = ("hello_ok", "login_ok", "same_instance", "aoi_enter_ok", "aoi_move_ok", "logout_leave_ok")
missing = [k for k in need if not r.get(k)]
if missing:
    print("ERROR: result.json missing/false", missing)
    sys.exit(1)
print("unity result.json assertions ok")
PY

echo "report_dir=$OUT_DIR"
echo "unity_two_clients=PASS"
echo "test_unity_two_clients.sh PASS"
