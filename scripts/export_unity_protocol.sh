#!/usr/bin/env bash
# 导出 Unity 联调用的外网协议：game.proto + descriptor set + SHA-256 manifest。
# 用法：./scripts/export_unity_protocol.sh <output_dir>
# 不写死 Unity 仓库路径；缺少 protoc 时非零退出。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-}"
if [[ -z "$OUT" ]]; then
  echo "usage: $0 <output_dir>" >&2
  exit 1
fi
if ! command -v protoc >/dev/null 2>&1; then
  echo "protoc not found" >&2
  exit 1
fi
if [[ ! -f "$ROOT/proto/game.proto" ]]; then
  echo "missing $ROOT/proto/game.proto" >&2
  exit 1
fi
mkdir -p "$OUT"
cp "$ROOT/proto/game.proto" "$OUT/game.proto"
protoc -I "$ROOT/proto" --descriptor_set_out="$OUT/game.desc" --include_imports \
  "$ROOT/proto/game.proto"

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  fi
}

HASH="$(sha256_file "$OUT/game.proto")"
DESC_HASH="$(sha256_file "$OUT/game.desc")"
GIT="unknown"
if command -v git >/dev/null 2>&1 && git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1; then
  GIT="$(git -C "$ROOT" rev-parse HEAD)"
fi
TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
cat > "$OUT/protocol_manifest.json" <<EOF
{
  "protocol_version": "game.v1",
  "git_sha": "$GIT",
  "frame": "uint32_be_length_prefixed",
  "max_frame_bytes": 4194304,
  "schema_files": ["game.proto"],
  "game_proto_sha256": "$HASH",
  "descriptor_set_sha256": "$DESC_HASH",
  "csharp_namespace": "GameMesh.Protocol",
  "package": "game",
  "generated_at_utc": "$TS",
  "push_message_types": ["aoi.delta.v1", "mailbox.changed.v1", "player.state.v1"]
}
EOF
printf '%s\n' "$HASH" > "$OUT/game.proto.sha256"
printf '%s\n' "$DESC_HASH" > "$OUT/game.desc.sha256"
echo "exported protocol to $OUT"
echo "game_proto_sha256=$HASH"
echo "descriptor_set_sha256=$DESC_HASH"
