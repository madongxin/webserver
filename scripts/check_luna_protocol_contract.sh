#!/usr/bin/env bash
# 跨仓库 Unity 协议门禁：对比服务器 proto/manifest 与 luna 客户端契约。
# 用法：
#   LUNA_REPO=/path/to/luna ./scripts/check_luna_protocol_contract.sh
#   ./scripts/check_luna_protocol_contract.sh /path/to/luna
#   GAMEMESH_REQUIRE_LUNA_CONTRACT=1 ./scripts/check_luna_protocol_contract.sh
#
# 行为：
# - LUNA_REPO 或参数给出仓库时：必须运行 Unity 检查脚本，hash 不一致则失败。
# - GAMEMESH_REQUIRE_LUNA_CONTRACT=1 且未给出仓库：失败（client_ready / 发布门禁）。
# - 未要求且未给出仓库：打印 NOT RUN，退出 0（普通单元测试）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REQUIRE="${GAMEMESH_REQUIRE_LUNA_CONTRACT:-0}"
LUNA="${LUNA_REPO:-}"
if [[ "${1:-}" != "" && "${1}" != --* ]]; then
  LUNA="$1"
fi
if [[ "${1:-}" == "--required" ]]; then
  REQUIRE=1
  if [[ "${2:-}" != "" ]]; then
    LUNA="$2"
  fi
fi

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  fi
}

not_run() {
  echo "NOT RUN luna protocol contract: $*"
  echo "luna_protocol_contract=NOT_RUN"
  exit 0
}

die() {
  echo "ERROR: luna protocol contract: $*" >&2
  echo "luna_protocol_contract=FAIL" >&2
  exit 1
}

if [[ -z "$LUNA" ]]; then
  if [[ "$REQUIRE" == "1" ]]; then
    die "LUNA_REPO unset; client_ready/release gates require Unity contract"
  fi
  not_run "LUNA_REPO unset"
fi

if [[ ! -d "$LUNA" ]]; then
  if [[ "$REQUIRE" == "1" ]]; then
    die "LUNA_REPO is not a directory: $LUNA"
  fi
  not_run "LUNA_REPO missing: $LUNA"
fi
LUNA="$(cd "$LUNA" && pwd)"

SERVER_COMMIT="unknown"
if git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1; then
  SERVER_COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
fi
LUNA_COMMIT="unknown"
if git -C "$LUNA" rev-parse HEAD >/dev/null 2>&1; then
  LUNA_COMMIT="$(git -C "$LUNA" rev-parse HEAD)"
fi

SERVER_PROTO="$ROOT/proto/game.proto"
CLIENT_PROTO=""
for cand in \
  "$LUNA/Assets/GameMesh/Protocol/Schema/game.proto" \
  "$LUNA/Assets/GameMesh/Protocol/game.proto" \
  "$LUNA/proto/game.proto"; do
  if [[ -f "$cand" ]]; then
    CLIENT_PROTO="$cand"
    break
  fi
done
CLIENT_MANIFEST=""
for cand in \
  "$LUNA/Assets/GameMesh/Protocol/protocol_manifest.json" \
  "$LUNA/protocol_manifest.json"; do
  if [[ -f "$cand" ]]; then
    CLIENT_MANIFEST="$cand"
    break
  fi
done

[[ -f "$SERVER_PROTO" ]] || die "missing $SERVER_PROTO"
SERVER_SCHEMA_SHA="$(sha256_file "$SERVER_PROTO")"
CLIENT_SCHEMA_SHA="missing"
CLIENT_MANIFEST_SHA="missing"
if [[ -n "$CLIENT_PROTO" ]]; then
  CLIENT_SCHEMA_SHA="$(sha256_file "$CLIENT_PROTO")"
fi
if [[ -n "$CLIENT_MANIFEST" ]]; then
  CLIENT_MANIFEST_SHA="$(python3 - "$CLIENT_MANIFEST" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
print((m.get("schema_sha256") or m.get("game_proto_sha256") or "missing").lower())
PY
)"
fi

echo "server_commit=$SERVER_COMMIT"
echo "luna_commit=$LUNA_COMMIT"
echo "server_schema_sha256=$SERVER_SCHEMA_SHA"
echo "client_schema_sha256=$CLIENT_SCHEMA_SHA"
echo "client_manifest_schema_sha256=$CLIENT_MANIFEST_SHA"

CHECK="$LUNA/Tools/GameMesh/check_protocol_contract.sh"
if [[ ! -f "$CHECK" ]]; then
  die "missing Unity checker $CHECK (luna_commit=$LUNA_COMMIT)"
fi
if [[ ! -x "$CHECK" ]]; then
  chmod +x "$CHECK" 2>/dev/null || true
fi

set +e
"$CHECK" "$ROOT"
rc=$?
set -e
if [[ "$rc" -ne 0 ]]; then
  die "Unity check_protocol_contract.sh rc=$rc server_commit=$SERVER_COMMIT luna_commit=$LUNA_COMMIT server_schema_sha256=$SERVER_SCHEMA_SHA client_schema_sha256=$CLIENT_SCHEMA_SHA"
fi

if [[ "$CLIENT_SCHEMA_SHA" != "$SERVER_SCHEMA_SHA" ]]; then
  die "schema hash mismatch after Unity checker server=$SERVER_SCHEMA_SHA client=$CLIENT_SCHEMA_SHA"
fi
if [[ "$CLIENT_MANIFEST_SHA" != "missing" && "$CLIENT_MANIFEST_SHA" != "$SERVER_SCHEMA_SHA" ]]; then
  die "client manifest schema_sha256 mismatch server=$SERVER_SCHEMA_SHA manifest=$CLIENT_MANIFEST_SHA"
fi

echo "luna_protocol_contract=PASS"
echo "check_luna_protocol_contract.sh PASS"
