#!/usr/bin/env bash
# 导出 Unity 联调用的外网协议：game.proto + descriptor set + SHA-256 manifest。
# 用法：./scripts/export_unity_protocol.sh <output_dir>
# 缺少 protoc、导出失败或 hash 不一致时非零退出。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-}"
if [[ -z "$OUT" ]]; then
  echo "usage: $0 <output_dir>" >&2
  exit 1
fi
if ! command -v protoc >/dev/null 2>&1; then
  echo "ERROR: protoc not found" >&2
  exit 1
fi
if [[ ! -f "$ROOT/proto/game.proto" ]]; then
  echo "ERROR: missing $ROOT/proto/game.proto" >&2
  exit 1
fi
mkdir -p "$OUT"
cp "$ROOT/proto/game.proto" "$OUT/game.proto"
protoc -I "$ROOT/proto" --descriptor_set_out="$OUT/game.desc" --include_imports \
  "$ROOT/proto/game.proto"
[[ -s "$OUT/game.proto" && -s "$OUT/game.desc" ]] || {
  echo "ERROR: export produced empty files" >&2
  exit 1
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  fi
}

HASH="$(sha256_file "$OUT/game.proto")"
DESC_HASH="$(sha256_file "$OUT/game.desc")"
[[ ${#HASH} -eq 64 && ${#DESC_HASH} -eq 64 ]] || {
  echo "ERROR: invalid sha256 length" >&2
  exit 1
}

GIT="unknown"
DIRTY="false"
if command -v git >/dev/null 2>&1 && git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1; then
  GIT="$(git -C "$ROOT" rev-parse HEAD)"
  if [[ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null || true)" ]]; then
    DIRTY="true"
  fi
fi
TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# 权威字段 + 旧别名（git_sha / game_proto_sha256 / frame）兼容既有解析脚本
cat > "$OUT/protocol_manifest.json" <<EOF
{
  "protocol_name": "gamemesh.game",
  "protocol_version": 1,
  "min_supported_protocol_version": 1,
  "server_git_sha": "$GIT",
  "git_sha": "$GIT",
  "dirty": $DIRTY,
  "schema_file": "game.proto",
  "schema_files": ["game.proto"],
  "schema_sha256": "$HASH",
  "game_proto_sha256": "$HASH",
  "descriptor_file": "game.desc",
  "descriptor_sha256": "$DESC_HASH",
  "descriptor_set_sha256": "$DESC_HASH",
  "frame_format": "uint32_be_length_prefixed",
  "frame": "uint32_be_length_prefixed",
  "max_frame_bytes": 4194304,
  "package": "game",
  "csharp_namespace": "GameMesh.Protocol",
  "generated_at_utc": "$TS",
  "push_message_types": ["aoi.delta.v1", "mailbox.changed.v1", "player.state.v1"]
}
EOF
printf '%s\n' "$HASH" > "$OUT/game.proto.sha256"
printf '%s\n' "$DESC_HASH" > "$OUT/game.desc.sha256"

check_hash="$(sha256_file "$OUT/game.proto")"
check_desc="$(sha256_file "$OUT/game.desc")"
[[ "$check_hash" == "$HASH" && "$check_desc" == "$DESC_HASH" ]] || {
  echo "ERROR: exported file hash mismatch" >&2
  exit 1
}
python3 - "$OUT/protocol_manifest.json" "$HASH" "$DESC_HASH" <<'PY' || exit 1
import json, sys
p, h, d = sys.argv[1], sys.argv[2], sys.argv[3]
m = json.load(open(p))
assert m["protocol_name"] == "gamemesh.game"
assert int(m["protocol_version"]) == 1
assert m["schema_sha256"] == h == m["game_proto_sha256"]
assert m["descriptor_sha256"] == d == m["descriptor_set_sha256"]
assert m["csharp_namespace"] == "GameMesh.Protocol"
assert m["package"] == "game"
assert m["frame_format"] == "uint32_be_length_prefixed"
assert int(m["max_frame_bytes"]) == 4194304
assert m["schema_file"] == "game.proto"
assert m["descriptor_file"] == "game.desc"
print("manifest ok")
PY

echo "exported protocol to $OUT"
echo "game_proto_sha256=$HASH"
echo "descriptor_set_sha256=$DESC_HASH"
echo "server_git_sha=$GIT dirty=$DIRTY"
