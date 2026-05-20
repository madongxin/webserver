#!/usr/bin/env bash
#
# run_webserver.sh — 配置/编译/启动 webserver（HTTP + 可选游戏 Protobuf + Redis + MySQL）
#
#   ./run_webserver.sh [HTTP_PORT] [GAME_PROTOBUF_PORT]
#   USE_MYSQL=0 ./run_webserver.sh
#   USE_GAME_PROTOBUF=0 ./run_webserver.sh
#   USE_REDIS=0 ./run_webserver.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "[run_webserver] build dir: $ROOT"

USE_ASAN="${USE_ASAN:-0}"
case "${USE_ASAN}" in 1|true|yes|on|ON) USE_ASAN=1 ;; *) USE_ASAN=0 ;; esac

# 默认开启 MySQL 监控落库；USE_MYSQL=0 可关闭
if [[ ! -f "${ROOT}/config/mysql.cnf" && -f "${ROOT}/config/mysql.cnf.example" ]]; then
  cp "${ROOT}/config/mysql.cnf.example" "${ROOT}/config/mysql.cnf"
  echo "[run_webserver] created config/mysql.cnf from example — edit ip/user/password/dbname"
fi
if [[ -z "${USE_MYSQL+x}" ]]; then
  USE_MYSQL=1
fi
USE_MYSQL="${USE_MYSQL:-1}"
case "${USE_MYSQL}" in 1|true|yes|on|ON) USE_MYSQL=1 ;; *) USE_MYSQL=0 ;; esac
if [[ "$USE_MYSQL" == "1" ]]; then
  echo "[run_webserver] MySQL metrics ON (metrics.webserver_metrics every 10s)"
else
  echo "[run_webserver] MySQL metrics OFF (USE_MYSQL=0)"
fi

USE_GAME_PROTOBUF="${USE_GAME_PROTOBUF:-1}"
case "${USE_GAME_PROTOBUF}" in 1|true|yes|on|ON) USE_GAME_PROTOBUF=1 ;; *) USE_GAME_PROTOBUF=0 ;; esac

# 默认开启 Redis 会话（游戏网关开启时）；USE_REDIS=0 可关闭
if [[ ! -f "${ROOT}/config/redis.cnf" && -f "${ROOT}/config/redis.cnf.example" ]]; then
  cp "${ROOT}/config/redis.cnf.example" "${ROOT}/config/redis.cnf"
  echo "[run_webserver] created config/redis.cnf from example (set password= if Redis requires auth)"
fi
if [[ -z "${USE_REDIS+x}" ]]; then
  USE_REDIS=1
fi
USE_REDIS="${USE_REDIS:-1}"
case "${USE_REDIS}" in 1|true|yes|on|ON) USE_REDIS=1 ;; *) USE_REDIS=0 ;; esac
if [[ "$USE_GAME_PROTOBUF" == "1" && "$USE_REDIS" == "1" ]]; then
  echo "[run_webserver] Redis session ON (game:session:{uid})"
fi

while [[ $# -gt 0 && "${1:0:1}" == "-" ]]; do
  case "$1" in
    --asan) USE_ASAN=1; shift ;;
    -h|--help)
      cat <<'EOF'
./run_webserver.sh [HTTP_PORT] [GAME_PORT]
Defaults: HTTP 8080, GAME HTTP+1.
Defaults: USE_MYSQL=1 USE_REDIS=1 USE_GAME_PROTOBUF=1 (USE_*=0 to disable)
EOF
      exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

PORT="${1:-8080}"
GAME_PORT="${2:-${GAME_PORT:-$((PORT + 1))}}"
WEBSERVER_LOG="${ROOT}/webserver-${PORT}.log"

_enable_game=OFF
_enable_redis=OFF
if [[ "$USE_GAME_PROTOBUF" == "1" ]]; then
  _enable_game=ON
  echo "[run_webserver] Game protobuf ON"
  if [[ "$USE_REDIS" == "1" ]]; then
    _enable_redis=ON
  else
    echo "[run_webserver] Redis session OFF (USE_REDIS=0)"
  fi
fi

_enable_mysql=OFF
if [[ "$USE_MYSQL" == "1" ]]; then
  _enable_mysql=ON
fi

_cmake_common=(
  -DENABLE_GAME_PROTOBUF="${_enable_game}"
  -DENABLE_REDIS="${_enable_redis}"
  -DENABLE_MYSQL="${_enable_mysql}"
)

if [[ "$USE_ASAN" == "1" ]]; then
  cmake -DENABLE_ASAN=ON "${_cmake_common[@]}" .
else
  cmake -DENABLE_ASAN=OFF "${_cmake_common[@]}" .
fi

make -j"$(nproc 2>/dev/null || echo 4)" webserver

# 释放端口，避免旧 webserver 占用导致新进程 bind 失败、客户端连到旧二进制
_stop_listen_port() {
  local p="$1"
  if command -v fuser >/dev/null 2>&1; then
    fuser -k "${p}/tcp" 2>/dev/null || true
  fi
}
if pgrep -f "${ROOT}/build/test/webserver" >/dev/null 2>&1; then
  echo "[run_webserver] stopping previous webserver ..."
  pkill -f "${ROOT}/build/test/webserver" 2>/dev/null || true
  sleep 1
fi
_stop_listen_port "${PORT}"
if [[ "$USE_GAME_PROTOBUF" == "1" ]]; then
  _stop_listen_port "${GAME_PORT}"
fi

ulimit -c $((200 * 1024)) 2>/dev/null || true

{
  echo ""
  echo "[run_webserver] --- $(date -Iseconds) HTTP=${PORT} GAME=${GAME_PORT} MYSQL=${USE_MYSQL} REDIS=${USE_REDIS} ---"
} >>"${WEBSERVER_LOG}"

echo "[run_webserver] log: ${WEBSERVER_LOG}"
echo "[run_webserver] app log (LOG_INFO): ${ROOT}/log/webserver.log"
exec > >(umask 022; exec tee -a "${WEBSERVER_LOG}") 2>&1

_lan_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo "[run_webserver] starting HTTP=${PORT} game_tcp=${GAME_PORT:-n/a}"
echo "[run_webserver] 在【服务器本机】访问: http://127.0.0.1:${PORT}/"
if [[ -n "${_lan_ip}" ]]; then
  echo "[run_webserver] 在【你的电脑浏览器】访问: http://${_lan_ip}:${PORT}/  (不要用 127.0.0.1，那是你本机)"
  echo "[run_webserver] 若外网打不开，请在云安全组/防火墙放行 TCP ${PORT} 和 ${GAME_PORT:-$((PORT+1))}"
fi
echo "[run_webserver] 或 SSH 端口转发: ssh -L ${PORT}:127.0.0.1:${PORT} user@<服务器>  然后浏览器开 http://127.0.0.1:${PORT}/"

if [[ "$USE_GAME_PROTOBUF" == "1" ]]; then
  exec "${ROOT}/build/test/webserver" "$PORT" "$GAME_PORT"
else
  exec "${ROOT}/build/test/webserver" "$PORT"
fi
