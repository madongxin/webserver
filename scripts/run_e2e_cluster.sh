#!/usr/bin/env bash
# 最终 E2E 专用集群（高端口，避免与 formal 8080 冲突）
# 拓扑: 2×gw + 2×session + 2×logic + 2×gamedb + world
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$ROOT/run/e2e}"
export GAMEMESH_ADVERTISE_HOST="${GAMEMESH_ADVERTISE_HOST:-127.0.0.1}"
export GAMEMESH_FORMAL="${GAMEMESH_FORMAL:-1}"
export GAMEMESH_HTTP_BIND="${GAMEMESH_HTTP_BIND:-127.0.0.1}"
export GAMEMESH_DRAIN_SEC="${GAMEMESH_DRAIN_SEC:-1}"
# 实验能力默认关闭（稳定版不承诺）
export GAMEMESH_EXPERIMENTAL_PLACEMENT_RECOVERY="${GAMEMESH_EXPERIMENTAL_PLACEMENT_RECOVERY:-0}"
export GAMEMESH_EXPERIMENTAL_DYNAMIC_SCALE="${GAMEMESH_EXPERIMENTAL_DYNAMIC_SCALE:-0}"
export GAMEMESH_EXPERIMENTAL_REGISTRY_OUTAGE="${GAMEMESH_EXPERIMENTAL_REGISTRY_OUTAGE:-0}"
# Placement 自动恢复仅在 experimental=1 时由 ServerBootstrap 注册；下列变量仍可供实验开启时使用
export GAMEMESH_PLACEMENT_SCAN_COUNT="${GAMEMESH_PLACEMENT_SCAN_COUNT:-256}"
export GAMEMESH_PLACEMENT_RECOVER_IV="${GAMEMESH_PLACEMENT_RECOVER_IV:-2}"

# 端口
export GAMEMESH_HTTP_G0="${GAMEMESH_HTTP_G0:-19080}"
export GAMEMESH_GAME_G0="${GAMEMESH_GAME_G0:-19081}"
export GAMEMESH_HTTP_G1="${GAMEMESH_HTTP_G1:-19082}"
export GAMEMESH_GAME_G1="${GAMEMESH_GAME_G1:-19083}"
export GAMEMESH_HTTP_L0="${GAMEMESH_HTTP_L0:-19090}"
export GAMEMESH_LOGIC0="${GAMEMESH_LOGIC0:-19201}"
export GAMEMESH_HTTP_L1="${GAMEMESH_HTTP_L1:-19091}"
export GAMEMESH_LOGIC1="${GAMEMESH_LOGIC1:-19202}"
export GAMEMESH_HTTP_W="${GAMEMESH_HTTP_W:-19092}"
export GAMEMESH_WORLD="${GAMEMESH_WORLD:-19301}"
export GAMEMESH_HTTP_S="${GAMEMESH_HTTP_S:-19093}"
export GAMEMESH_SESSION="${GAMEMESH_SESSION:-19401}"
export GAMEMESH_HTTP_S2="${GAMEMESH_HTTP_S2:-19096}"
export GAMEMESH_SESSION2="${GAMEMESH_SESSION2:-19402}"
export GAMEMESH_HTTP_D0="${GAMEMESH_HTTP_D0:-19094}"
export GAMEMESH_GAMEDB0="${GAMEMESH_GAMEDB0:-19501}"
export GAMEMESH_HTTP_D1="${GAMEMESH_HTTP_D1:-19095}"
export GAMEMESH_GAMEDB1="${GAMEMESH_GAMEDB1:-19502}"

if [[ -f "$GAMEMESH_RUN_DIR/pids" ]]; then
  echo "e2e cluster already running at $GAMEMESH_RUN_DIR (stop: ./scripts/stop_e2e_cluster.sh)"
  exit 1
fi

# 复用 formal 启动器端口变量
./scripts/start_formal.sh
# 追加第二 Session（与 run_cluster_local 一致）
SESSION_BIN=""
if [[ -x "$ROOT/build/test/session" ]]; then
  SESSION_BIN="$ROOT/build/test/session"
elif [[ -x "$ROOT/build/test/server" ]]; then
  SESSION_BIN="$ROOT/build/test/server"
fi
mkdir -p "$GAMEMESH_RUN_DIR/logs"
if [[ -n "$SESSION_BIN" ]]; then
  # shellcheck disable=SC1090
  source "$ROOT/scripts/e2e_inventory.sh"
  if [[ "$(basename "$SESSION_BIN")" == "server" ]]; then
    GAMEMESH_INSTANCE_ID=sess-1 nohup "$SESSION_BIN" session "$GAMEMESH_HTTP_S2" "$GAMEMESH_SESSION2" \
      >"$GAMEMESH_RUN_DIR/logs/session2.log" 2>&1 &
  else
    GAMEMESH_INSTANCE_ID=sess-1 nohup "$SESSION_BIN" "$GAMEMESH_HTTP_S2" "$GAMEMESH_SESSION2" \
      >"$GAMEMESH_RUN_DIR/logs/session2.log" 2>&1 &
  fi
  pid=$!
  echo "$pid" >>"$GAMEMESH_RUN_DIR/pids"
  e2e_inv_append session sess-1 "$pid" "127.0.0.1:${GAMEMESH_SESSION2}" "$GAMEMESH_HTTP_S2" -
  # Gateway Auth RR 含 sess-1：须等端口就绪，否则首批 Register 会 Connection refused
  for _ in $(seq 1 50); do
    if (echo >/dev/tcp/127.0.0.1/"${GAMEMESH_SESSION2}") >/dev/null 2>&1; then
      break
    fi
    sleep 0.2
  done
  sleep 0.5
fi

# 写出客户端端口约定
cat >"$GAMEMESH_RUN_DIR/E2E_PORTS.env" <<EOF
export GAMEMESH_RUN_DIR="$GAMEMESH_RUN_DIR"
export E2E_GW0_HTTP=$GAMEMESH_HTTP_G0
export E2E_GW0_GAME=$GAMEMESH_GAME_G0
export E2E_GW1_HTTP=$GAMEMESH_HTTP_G1
export E2E_GW1_GAME=$GAMEMESH_GAME_G1
export E2E_S0_HTTP=$GAMEMESH_HTTP_S
export E2E_S1_HTTP=$GAMEMESH_HTTP_S2
export E2E_L0_HTTP=$GAMEMESH_HTTP_L0
export E2E_L1_HTTP=$GAMEMESH_HTTP_L1
export E2E_DB0_HTTP=$GAMEMESH_HTTP_D0
export E2E_DB1_HTTP=$GAMEMESH_HTTP_D1
export E2E_WORLD_HTTP=$GAMEMESH_HTTP_W
export GAMEMESH_HTTP_L0=$GAMEMESH_HTTP_L0
export GAMEMESH_LOGIC0=$GAMEMESH_LOGIC0
export GAMEMESH_HTTP_L1=$GAMEMESH_HTTP_L1
export GAMEMESH_LOGIC1=$GAMEMESH_LOGIC1
export GAMEMESH_GAME_G0=$GAMEMESH_GAME_G0
export GAMEMESH_GAME_G1=$GAMEMESH_GAME_G1
EOF

echo "run_e2e_cluster.sh PASS"
echo "  source $GAMEMESH_RUN_DIR/E2E_PORTS.env"
echo "  game ports: $GAMEMESH_GAME_G0 / $GAMEMESH_GAME_G1"

# 额外等待：双 Session + Gateway 热通道（失败不阻断启动，由调用方 warm 登录探测）
sleep 1

