#!/usr/bin/env bash
# 网关机上盯着游戏口。任一连接 Send-Q 连续超过阈值达到 hold 秒，就存一份快照。
# 全机共享冷却（默认 120s），不按对端逐条落盘。摘要只写热点连接；目录自动轮转。
#
#   ./scripts/dump_parked_sockets.sh --watch [输出目录]
# start_formal.sh 会以后台方式启动 --watch。停止集群时一起停。
#
# 环境变量（可选）:
#   GAMEMESH_GAME_PORTS        默认 "8081 8083"
#   GAMEMESH_PARK_SENDQ        默认 16384
#   GAMEMESH_PARK_HOLD_S       默认 5
#   GAMEMESH_PARK_DUMP_GAP_S   默认 120（全机冷却）
#   GAMEMESH_PARK_MAX_DUMPS    默认 20（每组 3 个文件，超出删最旧）
#   GAMEMESH_PARK_SUMMARY_TOP  默认 64（摘要最多写多少条热点）
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-}"
if [[ "$MODE" == "--watch" ]]; then
  OUT="${2:-${GAMEMESH_PARKED_SS_DIR:-$ROOT/run/formal/parked-ss}}"
  exec python3 - "$OUT" <<'PY'
import os, subprocess, time, sys, glob
out = sys.argv[1]
ports = set(os.environ.get("GAMEMESH_GAME_PORTS", "8081 8083").split())
thresh = int(os.environ.get("GAMEMESH_PARK_SENDQ", "16384"))
hold_s = float(os.environ.get("GAMEMESH_PARK_HOLD_S", "5"))
gap_s = float(os.environ.get("GAMEMESH_PARK_DUMP_GAP_S", "120"))
max_dumps = int(os.environ.get("GAMEMESH_PARK_MAX_DUMPS", "20"))
summary_top = int(os.environ.get("GAMEMESH_PARK_SUMMARY_TOP", "64"))
poll_s = float(os.environ.get("GAMEMESH_PARK_POLL_S", "2"))
os.makedirs(out, exist_ok=True)
episode_start = None
last_dump = 0.0

def run(args):
    try:
        return subprocess.check_output(args, stderr=subprocess.STDOUT)
    except Exception as exc:
        return ("无数据 %s\n" % exc).encode("utf-8", "replace")

def records(text):
    rows = []
    cur = None
    for line in text.splitlines():
        if not line or line.startswith("State"):
            continue
        if line[0] in " \t":
            if cur is not None:
                cur["info"].append(line)
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        cur = {"recv": parts[1], "send": parts[2], "local": parts[3], "peer": parts[4],
               "info": [], "raw": line}
        rows.append(cur)
    return rows

def port_of(addr):
    return addr.rsplit(":", 1)[-1]

def stamp():
    return time.strftime("%Y%m%d-%H%M%S")

def rotate():
    if max_dumps <= 0:
        return
    raws = sorted(glob.glob(os.path.join(out, "ss-tinm-*.txt")))
    while len(raws) > max_dumps:
        old = raws.pop(0)
        base = os.path.basename(old)
        # ss-tinm-TS.txt or legacy ss-tinm-TS-peer.txt
        ts = base[len("ss-tinm-"):].rsplit(".", 1)[0]
        siblings = set(glob.glob(os.path.join(out, "ss-tinm-%s*.txt" % ts)))
        siblings.update(glob.glob(os.path.join(out, "sendq-summary-%s*.txt" % ts)))
        siblings.update(glob.glob(os.path.join(out, "nstat-%s*.txt" % ts)))
        for path in siblings:
            try:
                os.remove(path)
            except OSError:
                pass
        raws = sorted(glob.glob(os.path.join(out, "ss-tinm-*.txt")))

def save(hot_rows):
    ts = stamp()
    raw_path = os.path.join(out, "ss-tinm-%s.txt" % ts)
    sum_path = os.path.join(out, "sendq-summary-%s.txt" % ts)
    ns_path = os.path.join(out, "nstat-%s.txt" % ts)
    with open(raw_path, "wb") as f:
        f.write(run(["ss", "-tinm"]))
    hot_sorted = sorted(hot_rows, key=lambda r: int(r["send"]), reverse=True)
    if summary_top > 0:
        hot_sorted = hot_sorted[:summary_top]
    with open(sum_path, "w") as f:
        f.write("time=%s\nthreshold=%s\nhot_count=%d\nshown=%d\n" % (
            ts, thresh, len(hot_rows), len(hot_sorted)))
        f.write("recv_q send_q local peer\n")
        for row in hot_sorted:
            f.write("%s %s %s %s\n" % (row["recv"], row["send"], row["local"], row["peer"]))
    ns = run(["nstat", "-a", "TcpRetransSegs", "TcpOutSegs", "TcpExtTCPLostRetransmit"])
    with open(ns_path, "wb") as f:
        f.write(b"# nstat -a (not -z; -z would clear kernel counters)\n")
        f.write(ns)
    rotate()
    sys.stdout.write("saved %s hot=%d\n" % (raw_path, len(hot_rows)))
    sys.stdout.flush()

while True:
    text = run(["ss", "-tin"]).decode("utf-8", "replace")
    rows = records(text)
    now = time.time()
    hot = []
    for row in rows:
        if port_of(row["local"]) not in ports:
            continue
        try:
            send_q = int(row["send"])
        except ValueError:
            continue
        if send_q > thresh:
            hot.append(row)
    if hot:
        if episode_start is None:
            episode_start = now
        if now - episode_start >= hold_s and now - last_dump >= gap_s:
            save(hot)
            last_dump = now
            episode_start = now
    else:
        episode_start = None
    time.sleep(poll_s)
PY
fi

echo "usage: $0 --watch [output-dir]" >&2
echo "start_formal.sh launches --watch. Do not ssh to the load generator." >&2
exit 2
