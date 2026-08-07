#!/usr/bin/env bash
# 正式一键启动（多开常驻）：
#   2×gateway + 2×gamelogic + 2×gamedb + 1×world + 1×session
# 启动结束后打印客户端可连接的 Gateway 端口。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/formal}"
mkdir -p "$RUN_DIR/logs"

# ---- 端口（可用环境变量覆盖）----
# Gateway ×2（★ 客户端连 GAME 口）
HTTP_G0="${GAMEMESH_HTTP_G0:-8080}"
GAME_G0="${GAMEMESH_GAME_G0:-8081}"
HTTP_G1="${GAMEMESH_HTTP_G1:-8082}"
GAME_G1="${GAMEMESH_GAME_G1:-8083}"

# GameLogic ×2
HTTP_L0="${GAMEMESH_HTTP_L0:-8090}"
LOGIC0="${GAMEMESH_LOGIC0:-8201}"
HTTP_L1="${GAMEMESH_HTTP_L1:-8091}"
LOGIC1="${GAMEMESH_LOGIC1:-8202}"

# World ×1 / Session ×1
HTTP_W="${GAMEMESH_HTTP_W:-8092}"
WORLD="${GAMEMESH_WORLD:-8301}"
HTTP_S="${GAMEMESH_HTTP_S:-8093}"
SESSION="${GAMEMESH_SESSION:-8401}"

# GameDB ×2
HTTP_D0="${GAMEMESH_HTTP_D0:-8094}"
GAMEDB0="${GAMEMESH_GAMEDB0:-8501}"
HTTP_D1="${GAMEMESH_HTTP_D1:-8095}"
GAMEDB1="${GAMEMESH_GAMEDB1:-8502}"

pick_bin() {
  local name="$1"
  if [[ -x "$ROOT/build/test/${name}" ]]; then
    echo "$ROOT/build/test/${name}"
  elif [[ -x "$ROOT/build/test/server" ]]; then
    echo "$ROOT/build/test/server"
  else
    echo ""
  fi
}

SESSION_BIN="$(pick_bin session)"
GAMEDB_BIN="$(pick_bin gamedb)"
WORLD_BIN="$(pick_bin world)"
LOGIC_BIN="$(pick_bin gamelogic)"
GW_BIN="$(pick_bin gateway)"

if [[ -z "$GW_BIN" ]]; then
  echo "ERROR: 未找到二进制，请先编译："
  echo "  ./scripts/build.sh Debug"
  exit 1
fi

if [[ -f "$RUN_DIR/pids" ]]; then
  echo "已有正式实例在跑（$RUN_DIR/pids）。请先："
  echo "  ./scripts/stop_formal.sh"
  exit 1
fi

detect_advertise_host() {
  if [[ -n "${GAMEMESH_ADVERTISE_HOST:-}" ]]; then
    echo "$GAMEMESH_ADVERTISE_HOST"
    return
  fi
  local ip=""
  ip="$(curl -fsS -m 1 http://100.100.100.200/latest/meta-data/eip-ipv4 2>/dev/null || true)"
  [[ -z "$ip" ]] && ip="$(curl -fsS -m 1 http://100.100.100.200/latest/meta-data/public-ipv4 2>/dev/null || true)"
  if [[ -z "$ip" ]]; then
    ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
  fi
  echo "${ip:-127.0.0.1}"
}

ADVERTISE_HOST="$(detect_advertise_host)"

backup_cnf() {
  local f="$1"
  [[ -f "$f" ]] || return 0
  [[ -f "$RUN_DIR/$(basename "$f").bak" ]] || cp -a "$f" "$RUN_DIR/$(basename "$f").bak"
}

GW_CNF="$ROOT/config/gateway.cnf"
WORLD_CNF="$ROOT/config/world.cnf"
SESSION_CNF="$ROOT/config/session.cnf"
GAMEDB_CNF="$ROOT/config/gamedb.cnf"
LOGIC_CNF="$ROOT/config/gamelogic.cnf"

backup_cnf "$GW_CNF"
backup_cnf "$WORLD_CNF"
backup_cnf "$SESSION_CNF"
backup_cnf "$GAMEDB_CNF"
backup_cnf "$LOGIC_CNF"

# 两套 Gateway 共用同一份路由；Logic/GameDB 多地址
printf '%s\n' \
  "logic_addrs=127.0.0.1:${LOGIC0},127.0.0.1:${LOGIC1}" \
  "logic_instance_ids=gl-0,gl-1" \
  "world_addrs=127.0.0.1:${WORLD}" \
  "session_addrs=127.0.0.1:${SESSION}" \
  "gamedb_addrs=127.0.0.1:${GAMEDB0},127.0.0.1:${GAMEDB1}" \
  "etcd_endpoints=" \
  "rpc_timeout_ms=3000" \
  "ssl_enable=0" >"$GW_CNF"

printf '%s\n' \
  "listen_addr=0.0.0.0:${WORLD}" \
  "idle_timeout_sec=30" \
  "gamedb_addrs=127.0.0.1:${GAMEDB0},127.0.0.1:${GAMEDB1}" \
  "ssl_enable=0" >"$WORLD_CNF"

printf '%s\n' \
  "listen_addr=0.0.0.0:${SESSION}" \
  "idle_timeout_sec=30" \
  "logic_instance_ids=gl-0,gl-1" \
  "gamedb_addrs=127.0.0.1:${GAMEDB0},127.0.0.1:${GAMEDB1}" \
  "ssl_enable=0" >"$SESSION_CNF"

# gamedb/gamelogic 的 listen 以进程 argv 覆盖为准；cnf 写主实例便于手工启
printf '%s\n' \
  "listen_addr=0.0.0.0:${GAMEDB0}" \
  "idle_timeout_sec=30" \
  "nats_url=" \
  "ssl_enable=0" >"$GAMEDB_CNF"

printf '%s\n' \
  "listen_addr=0.0.0.0:${LOGIC0}" \
  "idle_timeout_sec=30" \
  "instance_id=gl-0" \
  "session_addrs=127.0.0.1:${SESSION}" \
  "gamedb_addrs=127.0.0.1:${GAMEDB0},127.0.0.1:${GAMEDB1}" \
  "ssl_enable=0" >"$LOGIC_CNF"

run_role() {
  local bin="$1" role="$2" logfile="$3"
  shift 3
  if [[ "$(basename "$bin")" == "server" ]]; then
    nohup "$bin" "$role" "$@" >"$logfile" 2>&1 &
  else
    nohup "$bin" "$@" >"$logfile" 2>&1 &
  fi
  echo $! >>"$RUN_DIR/pids"
  echo "  started $role pid=$! log=$logfile"
}

: >"$RUN_DIR/pids"

echo "== GameMesh formal start (2×gw + 2×logic + 2×gamedb + world + session) =="
run_role "$SESSION_BIN" session    "$RUN_DIR/logs/session.log" "$HTTP_S"  "$SESSION"
run_role "$GAMEDB_BIN"  gamedb0    "$RUN_DIR/logs/gamedb0.log" "$HTTP_D0" "$GAMEDB0"
run_role "$GAMEDB_BIN"  gamedb1    "$RUN_DIR/logs/gamedb1.log" "$HTTP_D1" "$GAMEDB1"
sleep 1
run_role "$WORLD_BIN"   world      "$RUN_DIR/logs/world.log"   "$HTTP_W"
run_role "$LOGIC_BIN"   gamelogic0 "$RUN_DIR/logs/logic0.log"  "$HTTP_L0" "$LOGIC0"
run_role "$LOGIC_BIN"   gamelogic1 "$RUN_DIR/logs/logic1.log"  "$HTTP_L1" "$LOGIC1"
sleep 1
run_role "$GW_BIN"      gateway0   "$RUN_DIR/logs/gw0.log"     "$HTTP_G0" "$GAME_G0"
run_role "$GW_BIN"      gateway1   "$RUN_DIR/logs/gw1.log"     "$HTTP_G1" "$GAME_G1"
sleep 2

fail=0
check_log() {
  local label="$1" file="$2" pattern="$3"
  if ! grep -qE "$pattern" "$file" 2>/dev/null; then
    echo "WARN: $label 未就绪，请看 $file"
    fail=1
  fi
}

check_log session    "$RUN_DIR/logs/session.log" 'SessionBrpcServer\(\+Auth\) listening|SessionBrpcServer listening|role=session'
check_log gamedb0    "$RUN_DIR/logs/gamedb0.log" 'GameDbBrpcServer listening|role=gamedb'
check_log gamedb1    "$RUN_DIR/logs/gamedb1.log" 'GameDbBrpcServer listening|role=gamedb'
check_log world      "$RUN_DIR/logs/world.log"   'WorldBrpcServer listening|role=world'
check_log gamelogic0 "$RUN_DIR/logs/logic0.log"  'GameLogicBrpcServer listening|role=gamelogic'
check_log gamelogic1 "$RUN_DIR/logs/logic1.log"  'GameLogicBrpcServer listening|role=gamelogic'
check_log gateway0   "$RUN_DIR/logs/gw0.log"     'GameTcpGateway ready|Game protobuf TCP'
check_log gateway1   "$RUN_DIR/logs/gw1.log"     'GameTcpGateway ready|Game protobuf TCP'
check_log gw0_push   "$RUN_DIR/logs/gw0.log"     'GatewayPushServer listening'
check_log gw1_push   "$RUN_DIR/logs/gw1.log"     'GatewayPushServer listening'

PUSH_G0=$((GAME_G0 + 100))
PUSH_G1=$((GAME_G1 + 100))

cat >"$RUN_DIR/CLIENT.txt" <<EOF
# Windows / 测试客户端连接信息（GameMesh Gateway ×2）
Host=${ADVERTISE_HOST}
Port=${GAME_G0}
PortAlt=${GAME_G1}
# 任选一个 Gateway 游戏口；生产建议 L4 LB/VIP，勿写死单机端口
# 安全组只放行 GameTCP ${GAME_G0},${GAME_G1}（HTTP/Push 不对公网）
# 内网 Push Logic→Gateway: ${PUSH_G0}/${PUSH_G1}
Protocol=TCP ProtoFraming + game.proto
LoginPath=Client→Gateway→Auth→Session.AcquireSession→GameLogic.BindPlayer
Topology=2x gateway, 2x gamelogic, 2x gamedb, 1x world(GlobalService), 1x session(+Auth)
EOF

echo
echo "=============================================="
echo "  客户端请连接 Gateway（二选一）"
echo "  Host  : ${ADVERTISE_HOST}"
echo "  Port0 : ${GAME_G0}   ← 推荐默认"
echo "  Port1 : ${GAME_G1}"
echo "  安全组只需 TCP ${GAME_G0},${GAME_G1}"
echo "  登录: Gateway→Auth→Session→BindPlayer"
echo "  Push内网: ${PUSH_G0}/${PUSH_G1}（勿对公网）"
echo "=============================================="
echo
echo "拓扑: 2×gateway + 2×gamelogic + 2×gamedb + world(GlobalService) + session(+Auth)"
echo "连接信息: $RUN_DIR/CLIENT.txt"
echo "日志目录: $RUN_DIR/logs/"
echo "停止命令: ./scripts/stop_formal.sh  或  ./scripts/stop_local.sh"
echo
if [[ "$fail" -ne 0 ]]; then
  echo "部分角色可能未就绪，请检查上方 WARN 与日志。"
  exit 1
fi
echo "全部角色已后台启动。"
