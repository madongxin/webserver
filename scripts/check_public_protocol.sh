#!/usr/bin/env bash
# 校验公网协议事实源：proto ↔ 生成代码、descriptor、字段号冻结、C# namespace、必需类型。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

die() { echo "ERROR: $*" >&2; exit 1; }

[[ -f proto/game.proto ]] || die "missing proto/game.proto"
[[ -f game/game.pb.h ]] || die "missing game/game.pb.h"
command -v protoc >/dev/null 2>&1 || die "protoc not found"

grep -q 'option csharp_namespace = "GameMesh.Protocol";' proto/game.proto \
  || die "csharp_namespace must be GameMesh.Protocol"
grep -q '^package game;' proto/game.proto || die "package game missing"

required_msgs=(
  RegisterReq LoginReq LogoutReq ReconnectReq
  PlayerAttributes EnterMapReq EnterMapRsp MoveReq MoveRsp
  AoiDelta PlayerMailSendReq MailboxChangedNotify ServerPushEnvelope
  ClientHelloReq ServerHelloRsp HeartbeatReq HeartbeatRsp
  WorldSnapshotReq FullStateSnapshotRsp RespawnReq RespawnRsp
  ChatSendReq ChatNotify GetPlayerBriefReq QueryOnlineStateReq PlayerBrief
  SessionReplacedNotify MapManifestEntry
)
for m in "${required_msgs[@]}"; do
  grep -q "message $m " proto/game.proto || die "missing message $m"
done

# 已发布 oneof / 关键字段号不得漂移（与 client_protocol_test 对齐）
python3 - <<'PY' || exit 1
import re, sys
text = open("proto/game.proto").read()

def field(msg, name, num):
    # crude: find message block then field
    m = re.search(r"message %s \{.*?\n\}" % msg, text, re.S)
    if not m:
        print("missing message", msg); sys.exit(1)
    pat = r"\b%s\s*=\s*%d\s*;" % (re.escape(name), num)
    if not re.search(pat, m.group(0)):
        print("field drift", msg, name, num); sys.exit(1)

field("GameRequest", "login", 20)
field("GameRequest", "logout", 23)
field("GameRequest", "reconnect", 24)
field("GameRequest", "register", 25)
field("GameRequest", "enter_map", 40)
field("GameRequest", "get_self_profile", 60)
field("GameRequest", "move", 61)
field("GameRequest", "player_mail_send", 63)
field("GameRequest", "client_hello", 70)
field("GameRequest", "heartbeat", 71)
field("GameRequest", "world_snapshot", 72)
field("GameRequest", "respawn", 73)
field("GameRequest", "get_player_brief", 74)
field("GameRequest", "query_online_state", 75)
field("GameResponse", "login", 20)
field("GameResponse", "error_code", 4)
field("GameResponse", "retryable", 5)
field("GameResponse", "server_time_ms", 6)
field("GameResponse", "trace_id", 7)
field("GameResponse", "server_push", 53)
field("GameResponse", "full_snapshot", 54)
field("GameResponse", "server_hello", 70)
field("GameResponse", "heartbeat", 71)
field("GameResponse", "respawn", 72)
field("GameResponse", "chat_notify", 73)
field("GameResponse", "get_player_brief", 74)
field("GameResponse", "query_online_state", 75)
field("GameResponse", "session_replaced", 76)
field("ServerHelloRsp", "gameplay_config_version", 12)
field("ServerHelloRsp", "map_manifest_version", 13)
field("ServerHelloRsp", "maps", 14)
field("SessionReplacedNotify", "reason_code", 1)
field("SessionReplacedNotify", "server_time_ms", 2)
field("FullStateSnapshotRsp", "baseline_server_seq", 7)
field("FullStateSnapshotRsp", "profile", 8)
field("FullStateSnapshotRsp", "aoi_entities", 16)
field("PlayerAttributes", "life_state", 16)
field("GameResponse", "get_self_profile", 60)
field("GameResponse", "move", 61)
field("GameResponse", "aoi_delta", 62)
field("GameResponse", "player_mail_send", 63)
field("GameResponse", "mailbox_changed", 64)
field("LoginRsp", "profile", 9)
field("EnterMapReq", "map_data_sha256", 6)
field("EnterMapRsp", "aoi_snapshot", 13)
field("PlayerAttributes", "stats_version", 15)
print("frozen fields ok")
PY

# pb.h 含生成标记与关键字段号
grep -q "kLoginFieldNumber = 20" game/game.pb.h || die "game.pb.h login field != 20"
grep -q "kMoveFieldNumber = 61" game/game.pb.h || die "game.pb.h move field != 61"
grep -q "kPlayerMailSendFieldNumber = 63" game/game.pb.h || die "game.pb.h player_mail_send != 63"
grep -q "kClientHelloFieldNumber = 70" game/game.pb.h || die "game.pb.h client_hello != 70"
grep -q "kWorldSnapshotFieldNumber = 72" game/game.pb.h || die "game.pb.h world_snapshot != 72"
grep -q "kSessionReplacedFieldNumber = 76" game/game.pb.h || die "game.pb.h session_replaced != 76"
grep -q "kErrorCodeFieldNumber = 4" game/game.pb.h || die "game.pb.h error_code != 4"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$ROOT/scripts/export_unity_protocol.sh" "$TMP/out"
[[ -s "$TMP/out/game.proto" ]] || die "export missing game.proto"
[[ -s "$TMP/out/game.desc" ]] || die "export missing game.desc"
[[ -s "$TMP/out/protocol_manifest.json" ]] || die "export missing manifest"
[[ -s "$TMP/out/game.proto.sha256" ]] || die "export missing game.proto.sha256"
[[ -s "$TMP/out/game.desc.sha256" ]] || die "export missing game.desc.sha256"
python3 - "$TMP/out/protocol_manifest.json" "$ROOT" <<'PY' || exit 1
import json, subprocess, sys
m = json.load(open(sys.argv[1]))
root = sys.argv[2]
need = [
    "server_commit", "protocol_version", "min_supported_protocol_version",
    "schema_sha256", "descriptor_sha256", "frame_format", "max_frame_bytes",
    "protoc_version", "required_types",
]
missing = [k for k in need if k not in m]
if missing:
    print("ERROR: manifest missing", missing); sys.exit(1)
try:
    head = subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"]).decode().strip()
    if m["server_commit"] != head:
        print("ERROR: server_commit", m["server_commit"], "!= HEAD", head); sys.exit(1)
except Exception as e:
    print("ERROR: git HEAD", e); sys.exit(1)
req = set(m["required_types"])
for t in ("ClientHelloReq", "ServerHelloRsp", "PushAckReq", "MailboxChangedNotify",
          "SessionReplacedNotify", "MapManifestEntry"):
    if t not in req:
        print("ERROR: required_types missing", t); sys.exit(1)
print("export manifest fields ok")
PY

# descriptor 可被 protoc 解析
protoc --decode_raw < "$TMP/out/game.desc" >/dev/null \
  || die "game.desc is not a valid FileDescriptorSet"

# 与源 proto 内容一致
if ! cmp -s proto/game.proto "$TMP/out/game.proto"; then
  die "exported game.proto differs from proto/game.proto"
fi

PUB="$ROOT/docs/protocol/published/v1/game.desc"
[[ -f "$PUB" ]] || die "missing published descriptor $PUB"
COMPAT="$ROOT/build/test/protocol_compat_test"
if [[ -x "$COMPAT" ]]; then
  "$COMPAT" "$PUB" "$TMP/out/game.desc" || die "protocol_compat_test failed"
else
  python3 - "$PUB" "$TMP/out/game.desc" <<'PY' || exit 1
import os, sys
a, b = sys.argv[1], sys.argv[2]
assert os.path.getsize(a) > 32 and os.path.getsize(b) > 32
print("published descriptor present (compat binary missing)")
PY
fi

echo "check_public_protocol.sh PASS"
