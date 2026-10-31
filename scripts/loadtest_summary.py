#!/usr/bin/env python3
# 压测窗口汇总。缺的指标打印「缺失」，不用 0 代替。
# 用法：
#   scripts/loadtest_summary.py --start 2026-10-27T04:05:00Z --end 2026-10-27T04:14:30Z \
#       --bots 1000 --denominator move_replies
import argparse
import json
import subprocess
import urllib.parse
import urllib.request

PROM = {"url": "http://127.0.0.1:9090"}
MISSING = "缺失"

# Move 回包路径上的七个时刻。没有对应直方图时，分位数就是缺失。
STAGES = [
    "request_received",
    "logic_dispatch_started",
    "logic_response_ready",
    "event_loop_enqueued",
    "control_outbox_queued",
    "socket_write_accepted",
    "write_complete",
]


def git_sha():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], universal_newlines=True
        ).strip()
    except Exception:
        return MISSING


def prom(query, t):
    url = PROM["url"] + "/api/v1/query?" + urllib.parse.urlencode({"query": query, "time": str(t)})
    try:
        with urllib.request.urlopen(url, timeout=8) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except Exception:
        return None
    if body.get("status") != "success":
        return None
    return body.get("data", {}).get("result", [])


def scalar(rows):
    if not rows:
        return None
    total = 0.0
    for row in rows:
        try:
            total += float(row["value"][1])
        except (KeyError, TypeError, ValueError):
            return None
    return total


def delta(query, start, end):
    a = scalar(prom(query, start))
    b = scalar(prom(query, end))
    if a is None or b is None:
        return None
    return b - a


def fmt(v):
    if v is None:
        return MISSING
    if abs(v - round(v)) < 1e-6:
        return str(int(round(v)))
    return "%.4g" % v


def histogram_quantile(kind_query, start, end, q):
    """用窗口两端的累计桶做增量，再取桶上沿。没有桶就是缺失。"""
    les = ["5", "10", "20", "50", "100", "200", "500", "1000", "2000", "5000", "+Inf"]
    edges = []
    for le in les:
        d = delta(kind_query % le, start, end)
        if d is None:
            return None
        edges.append((le, d))
    total = edges[-1][1]
    if total <= 0:
        return "0"
    target = q * total
    for le, cum in edges:
        if cum + 1e-9 >= target:
            return "5001" if le == "+Inf" else le
    return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--start", required=True, help="unix seconds or RFC3339")
    p.add_argument("--end", required=True)
    p.add_argument("--bots", default=MISSING, help="机器人数。不知道就别填，会显示缺失")
    p.add_argument("--denominator", default=MISSING, help="分母的名字，例如 move_replies")
    p.add_argument("--prom", default="http://127.0.0.1:9090")
    args = p.parse_args()
    PROM["url"] = args.prom

    def parse_t(s):
        try:
            return int(float(s))
        except ValueError:
            import calendar
            import time
            return int(calendar.timegm(time.strptime(s.replace("Z", ""), "%Y-%m-%dT%H:%M:%S")))

    start = parse_t(args.start)
    end = parse_t(args.end)
    print("build_sha %s" % git_sha())
    print("window_start %s" % args.start)
    print("window_end %s" % args.end)
    print("bots %s" % (args.bots if args.bots else MISSING))
    print("denominator %s" % (args.denominator if args.denominator else MISSING))

    move_count = delta(
        'sum(gamemesh_reply_latency_ms_count{job="gamemesh",role="gateway",kind="move"})',
        start, end)
    print("move_reply_count %s" % fmt(move_count))
    e2e = (
        'sum(gamemesh_reply_latency_ms_bucket{job="gamemesh",role="gateway",kind="move",le="%s"})'
    )
    e2e_parts = []
    for name, q in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99)):
        edge = histogram_quantile(e2e, start, end, q)
        e2e_parts.append("%s=%s" % (name, edge if edge is not None else MISSING))
    print("move_reply_e2e_bucket_edge %s" % " ".join(e2e_parts))
    if args.denominator == "move_replies":
        print("denominator_value %s" % fmt(move_count))
    else:
        print("denominator_value %s" % MISSING)

    print("move_e2e_stages")
    bucket_q = (
        'sum(gamemesh_pkt_stage_ms_bucket{job="gamemesh",kind="move",stage="%s",le="%s"})'
    )
    for stage in STAGES:
        parts = []
        for name, q in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99)):
            edge = histogram_quantile(bucket_q % (stage, "%s"), start, end, q)
            parts.append("%s=%s" % (name, edge if edge is not None else MISSING))
        print("  %s %s" % (stage, " ".join(parts)))

    tx = delta(
        'sum(gamemesh_gateway_tx_bytes_total{job="gamemesh",role="gateway"})', start, end)
    span = end - start
    if tx is None or span <= 0:
        print("egress_mbps %s" % MISSING)
    else:
        print("egress_mbps %.4g" % (tx * 8.0 / span / 1e6))

    for label, query in (
        ("outbox_frames",
         'sum(gamemesh_control_outbox_current_frames{job="gamemesh",role="gateway"})'),
        ("aoi_backlog", 'sum(gamemesh_aoi_push_backlog{job="gamemesh"})'),
        ("send_pending_bytes",
         'max(gamemesh_send_pending_max_bytes{job="gamemesh",role="gateway"})'),
    ):
        print("%s %s" % (label, fmt(scalar(prom(query, end)))))

    for label, query in (
        ("move_drop", 'sum(gamemesh_move_drop_total{job="gamemesh",role="gateway"})'),
        ("move_coalesce", 'sum(gamemesh_move_coalesce_total{job="gamemesh",role="gateway"})'),
        ("aoi_shed", 'sum(gamemesh_aoi_move_shed_total{job="gamemesh"})'),
        ("aoi_skip_retrans",
         'sum(gamemesh_aoi_skip_retrans_total{job="gamemesh",role="gateway"})'),
    ):
        print("%s %s" % (label, fmt(delta(query, start, end))))


if __name__ == "__main__":
    main()
