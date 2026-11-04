#!/usr/bin/env bash
# 压测进行时抓 Gateway 游戏口上 Send-Q 超过 16KB 的连接。
# 保存 ss -tinm 原文，用来区分：对端窗口为 0、重传/拥塞退避，还是本机仍在写入。
# 本机没有机器人进程时，CPU 和接收缓冲写「无数据」，并列出对端地址。
#
# 用法：
#   ./scripts/dump_parked_sockets.sh [秒数] [输出目录]
# 可选：GAMEMESH_ROBOT_SSH=user@host  到压测机上取机器人进程的 CPU 和 ss。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECS="${1:-30}"
OUT="${2:-$ROOT/run/formal/parked-ss}"
PORTS="${GAMEMESH_GAME_PORTS:-8081 8083}"
THRESH="${GAMEMESH_PARK_SENDQ:-16384}"
mkdir -p "$OUT"
STAMP="$(date +%Y%m%d-%H%M%S)"
RAW="$OUT/ss-$STAMP.txt"
FILT="$OUT/sendq-over-${THRESH}-$STAMP.txt"
META="$OUT/meta-$STAMP.txt"

: >"$RAW"
end=$((SECONDS + SECS))
n=0
while (( SECONDS < end )); do
  n=$((n + 1))
  {
    echo "===== sample $n $(date -Is 2>/dev/null || date) ====="
    ss -tinm 2>/dev/null || ss -tin 2>/dev/null || true
  } >>"$RAW"
  sleep 2
done

python3 - "$RAW" "$FILT" "$THRESH" $PORTS <<'PY'
import sys
raw, filt, thresh = sys.argv[1], sys.argv[2], int(sys.argv[3])
ports = set(sys.argv[4:])
lines = open(raw, "r", errors="replace").read().splitlines()
out = []
i = 0
while i < len(lines):
    line = lines[i]
    if line.startswith("====="):
        out.append(line)
        i += 1
        continue
    parts = line.split()
    info = ""
    if i + 1 < len(lines) and lines[i + 1].startswith((" ", "\t")):
        info = lines[i + 1].strip()
    if len(parts) >= 5 and parts[0] not in ("State", "Recv-Q"):
        try:
            sq = int(parts[2])
        except ValueError:
            sq = -1
        local = parts[3]
        port = local.rsplit(":", 1)[-1]
        if sq > thresh and port in ports:
            out.append(line)
            if info:
                out.append(info)
            out.append("peer=%s sendq=%s recvq=%s" % (parts[4], parts[2], parts[1]))
        if info:
            i += 1
    i += 1
open(filt, "w").write("\n".join(out) + ("\n" if out else ""))
print("filtered_lines", len(out))
PY

{
  echo "time=$(date -Is 2>/dev/null || date)"
  echo "ports=$PORTS"
  echo "sendq_threshold=$THRESH"
  echo "seconds=$SECS"
  echo "samples=$n"
  echo "raw=$RAW"
  echo "filtered=$FILT"
  robots="$(ps -eo pid,pcpu,pmem,rss,nlwp,cmd 2>/dev/null | grep -E -i 'robot|loadtest|botclient|gameclient' | grep -v grep || true)"
  if [[ -z "$robots" ]]; then
    echo "local_robot_cpu=无数据"
    echo "local_robot_recv=无数据"
  else
    echo "local_robot_cpu:"
    echo "$robots"
  fi
  if [[ -z "${GAMEMESH_ROBOT_SSH:-}" ]] || ! command -v ssh >/dev/null 2>&1; then
    echo "remote_robot_cpu=无数据"
    echo "remote_robot_recv=无数据"
  else
    remote="$(ssh -o BatchMode=yes -o ConnectTimeout=3 "$GAMEMESH_ROBOT_SSH" \
      'ps -eo pid,pcpu,pmem,rss,nlwp,cmd | grep -E -i "robot|loadtest|botclient|gameclient" | grep -v grep; echo ---SS---; ss -tinm' \
      2>/dev/null || true)"
    if [[ -z "$remote" ]]; then
      echo "remote_robot_cpu=无数据"
      echo "remote_robot_recv=无数据"
    else
      echo "remote:"
      echo "$remote"
    fi
  fi
  echo "peers:"
  grep -E '^peer=' "$FILT" | sed 's/ sendq=.*//' | sort | uniq -c | sort -nr | head -40 || true
} >"$META"

echo "wrote $META"
echo "raw ss: $RAW"
echo "sendq>$THRESH: $FILT"
