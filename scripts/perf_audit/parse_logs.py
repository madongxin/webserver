#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""只读流式解析 formal 压测日志。禁止整文件载入内存。"""
from __future__ import print_function

import argparse
import csv
import json
import os
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime

TS_RE = re.compile(r"(\d{8} \d{2}:\d{2}:\d{2})")
PLAYER_RE = re.compile(r"player(?:_id)?[=:](\d+)")
CONN_RE = re.compile(r"conn[#=](\d+)")
MAP_RE = re.compile(r"map[=:](\d+)|inst[=:](\d+)")
LINE_RE = re.compile(r"req_line=(\d+)")
OK_RE = re.compile(r"ok=(\d+)")
BYTES_RE = re.compile(r"bytes=(\d+)")
ERR_RE = re.compile(r"ERR_[A-Z0-9_]+")
PUSH_RE = re.compile(
    r"PushBatch rejected=(\d+) accepted=(\d+) messages=(\d+)"
)


def parse_ts(line):
    m = TS_RE.search(line)
    if not m:
        return None
    return datetime.strptime(m.group(1), "%Y%m%d %H:%M:%S")


def in_window(t, start, end):
    if t is None:
        return False
    if start and t < start:
        return False
    if end and t > end:
        return False
    return True


def bucket10(t):
    return t.replace(second=(t.second // 10) * 10, microsecond=0)


def classify(line, service):
    """返回 (event, extra_dict) 或 None。"""
    if "ClientHello ok" in line:
        return "hello_ok", {}
    if "Gateway login orchestrated" in line:
        return "login_ok", {}
    if "Gateway reconnect orchestrated" in line:
        return "reconnect_ok", {}
    if "Gateway logout orchestrated" in line:
        return "logout", {}
    if "EnterMap resolve" in line:
        return "enter_resolve", {}
    if "EnterMap reply" in line:
        return "enter_reply", {}
    if "Gateway disconnect conn#" in line:
        return "disconnect", {}
    if "KickConnection" in line:
        return "kick", {}
    if "AcquireSession player_id=" in line:
        return "acquire_session", {}
    if "AuthService Login ok" in line:
        return "auth_login_ok", {}
    if "AuthService Register ok" in line:
        return "auth_register_ok", {}
    if "BindPlayer ok" in line:
        return "bind_ok", {}
    if "[enter_map] player=" in line:
        return "logic_enter", {}
    if "PushBatch rejected=" in line or "PushBatch" in line and "rejected=" in line:
        return "push_batch", {}
    if "ERR_MOVE_TOO_FAST" in line or "ERR_UNWALKABLE" in line or "ERR_STALE_SEQ" in line:
        return "move_err_log", {}
    if "SessionGraceSweeper" in line:
        return "grace_sweep", {}
    if "idle timeout" in line.lower():
        return "idle_timeout", {}
    if "ERR_OVERLOADED" in line:
        return "overloaded", {}
    if "illegal protobuf" in line or "invalid frame" in line:
        return "illegal_frame", {}
    if "missing sticky gamelogic" in line:
        return "missing_sticky", {}
    return None


def analyze(log_dir, start, end, out_dir):
    files = [
        ("gateway", "gw-0", "gw0.log"),
        ("gateway", "gw-1", "gw1.log"),
        ("gamelogic", "gl-0", "logic0.log"),
        ("gamelogic", "gl-1", "logic1.log"),
        ("session", "session-0", "session.log"),
        ("session", "session-1", "session1.log"),
        ("gamedb", "gamedb-0", "gamedb0.log"),
        ("gamedb", "gamedb-1", "gamedb1.log"),
        ("world", "world-0", "world.log"),
    ]

    events_10s = defaultdict(Counter)
    events_1s = defaultdict(Counter)
    errors_10s = defaultdict(Counter)
    players = {
        "login": set(),
        "enter": set(),
        "acquire": set(),
        "bind": set(),
        "disconnect": set(),
        "logout": set(),
    }
    enter_by_line = Counter()
    enter_by_map = Counter()
    enter_ok = Counter()
    push_stats = []
    push_rejected_batches = 0
    push_accepted_sum = 0
    push_rejected_sum = 0
    sample_events = []
    offsets = {}
    file_ranges = {}
    err_codes = Counter()
    login_ts = {}
    enter_ts = {}
    first_disc = {}
    req_line = {}

    events_csv = os.path.join(out_dir, "events_sample.csv")
    timeline_csv = os.path.join(out_dir, "timeline_10s.csv")

    with open(events_csv, "w") as ef:
        ew = csv.writer(ef)
        ew.writerow(
            [
                "ts_utc",
                "service",
                "instance",
                "host",
                "source_file",
                "line_or_offset",
                "level",
                "event",
                "run_id",
                "conn_id",
                "request_seq",
                "player_key",
                "session_generation",
                "map_instance_id",
                "error_code",
                "duration_ms",
                "bytes",
            ]
        )

        for service, instance, name in files:
            path = os.path.join(log_dir, name)
            if not os.path.isfile(path):
                continue
            first_t = last_t = None
            line_no = 0
            with open(path, "r", errors="replace") as f:
                while True:
                    offset = f.tell()
                    line = f.readline()
                    if not line:
                        break
                    line_no += 1
                    t = parse_ts(line)
                    if t:
                        if first_t is None:
                            first_t = t
                        last_t = t
                    if not in_window(t, start, end):
                        continue
                    cls = classify(line, service)
                    if not cls:
                        # still count error codes in window
                        for e in ERR_RE.findall(line):
                            err_codes[e] += 1
                            errors_10s[bucket10(t)][e] += 1
                        continue
                    event, _ = cls
                    pid_m = PLAYER_RE.search(line)
                    pid = int(pid_m.group(1)) if pid_m else None
                    conn_m = CONN_RE.search(line)
                    conn = conn_m.group(1) if conn_m else ""
                    map_id = ""
                    mm = MAP_RE.search(line)
                    if mm:
                        map_id = mm.group(1) or mm.group(2) or ""
                    ok_m = OK_RE.search(line)
                    bytes_m = BYTES_RE.search(line)
                    level = "ERROR" if " ERROR " in line or '"level":"ERROR"' in line else (
                        "WARN" if " WARN " in line or '"level":"WARN"' in line else "INFO"
                    )
                    err = ""
                    em = ERR_RE.search(line)
                    if em:
                        err = em.group(0)
                        err_codes[err] += 1
                        errors_10s[bucket10(t)][err] += 1

                    b10 = bucket10(t)
                    events_10s[b10][event] += 1
                    events_1s[t][event] += 1

                    if event == "login_ok" and pid:
                        players["login"].add(pid)
                        login_ts[pid] = t
                    elif event == "enter_reply" and pid:
                        players["enter"].add(pid)
                        enter_ts[pid] = t
                        if ok_m:
                            enter_ok[ok_m.group(1)] += 1
                        if map_id:
                            enter_by_map[map_id] += 1
                    elif event == "enter_resolve" and pid:
                        lm = LINE_RE.search(line)
                        if lm:
                            enter_by_line[int(lm.group(1))] += 1
                            req_line[pid] = int(lm.group(1))
                        if map_id:
                            enter_by_map["resolve->" + map_id] += 1
                    elif event == "acquire_session" and pid:
                        players["acquire"].add(pid)
                    elif event == "bind_ok" and pid:
                        players["bind"].add(pid)
                    elif event == "disconnect" and pid:
                        players["disconnect"].add(pid)
                        if pid not in first_disc:
                            first_disc[pid] = t
                    elif event == "logout" and pid:
                        players["logout"].add(pid)
                    elif event == "push_batch":
                        pm = PUSH_RE.search(line)
                        if pm:
                            rej, acc, msg = int(pm.group(1)), int(pm.group(2)), int(pm.group(3))
                            push_rejected_batches += 1 if rej else 0
                            push_rejected_sum += rej
                            push_accepted_sum += acc
                            push_stats.append((t.isoformat(), instance, rej, acc, msg))

                    if len(sample_events) < 5000 or event in (
                        "login_ok",
                        "enter_reply",
                        "overloaded",
                        "kick",
                    ):
                        row = [
                            t.strftime("%Y-%m-%dT%H:%M:%SZ"),
                            service,
                            instance,
                            "local",
                            path,
                            str(line_no),
                            level,
                            event,
                            "",
                            conn,
                            "",
                            str(pid) if pid else "",
                            "",
                            map_id,
                            err,
                            "",
                            bytes_m.group(1) if bytes_m else "",
                        ]
                        ew.writerow(row)
                        sample_events.append(row)
            file_ranges[name] = {
                "path": path,
                "first": first_t.isoformat() if first_t else None,
                "last": last_t.isoformat() if last_t else None,
                "size": os.path.getsize(path),
                "lines_scanned_note": "full file scanned streamingly",
            }
            offsets[name] = line_no

    # lifetime buckets for disconnect after login
    life = Counter()
    for pid, dt in first_disc.items():
        if pid in login_ts:
            sec = (dt - login_ts[pid]).total_seconds()
            if sec < 60:
                life["<1m"] += 1
            elif sec < 180:
                life["1-3m"] += 1
            elif sec < 300:
                life["3-5m"] += 1
            elif sec < 420:
                life["5-7m"] += 1
            elif sec < 900:
                life["7-15m"] += 1
            elif sec < 1500:
                life["15-25m"] += 1
            else:
                life[">25m"] += 1

    with open(timeline_csv, "w") as tf:
        tw = csv.writer(tf)
        keys = sorted(
            {
                k
                for c in events_10s.values()
                for k in c.keys()
            }
            | {"hello_ok", "login_ok", "enter_reply", "disconnect", "logout", "push_batch"}
        )
        tw.writerow(["ts_10s"] + keys)
        for b in sorted(events_10s.keys()):
            tw.writerow([b.strftime("%Y-%m-%dT%H:%M:%SZ")] + [events_10s[b].get(k, 0) for k in keys])

    # peak CCU proxy: login - disconnect cumulative (rough)
    peak_online = 0
    online = 0
    for b in sorted(events_10s.keys()):
        online += events_10s[b].get("login_ok", 0)
        online -= events_10s[b].get("disconnect", 0)
        if online > peak_online:
            peak_online = online

    summary = {
        "window_start": start.isoformat() if start else None,
        "window_end": end.isoformat() if end else None,
        "unique_login": len(players["login"]),
        "unique_acquire": len(players["acquire"]),
        "unique_bind": len(players["bind"]),
        "unique_enter_reply": len(players["enter"]),
        "unique_disconnect": len(players["disconnect"]),
        "unique_logout": len(players["logout"]),
        "enter_ok_counter": dict(enter_ok),
        "enter_by_req_line": dict(sorted(enter_by_line.items())),
        "enter_by_map_top": enter_by_map.most_common(20),
        "error_codes_in_window": err_codes.most_common(40),
        "disconnect_lifetime_buckets": dict(life),
        "push_rejected_sum": push_rejected_sum,
        "push_accepted_sum": push_accepted_sum,
        "push_batch_log_lines": len(push_stats),
        "push_batches_with_reject": push_rejected_batches,
        "peak_online_proxy_10s": peak_online,
        "file_ranges": file_ranges,
        "evidence": "OBSERVED from stream parse of run/formal/logs",
    }
    with open(os.path.join(out_dir, "summary.json"), "w") as sf:
        json.dump(summary, sf, indent=2, ensure_ascii=False)

    # error timeline
    with open(os.path.join(out_dir, "errors_10s.csv"), "w") as erf:
        ew2 = csv.writer(erf)
        codes = sorted({c for m in errors_10s.values() for c in m})
        ew2.writerow(["ts_10s"] + codes)
        for b in sorted(errors_10s.keys()):
            ew2.writerow([b.strftime("%Y-%m-%dT%H:%M:%SZ")] + [errors_10s[b].get(c, 0) for c in codes])

    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-dir", default="run/formal/logs")
    ap.add_argument("--out-dir", default="docs/perf_audit/data")
    ap.add_argument("--start", default="20261015 13:35:00")
    ap.add_argument("--end", default="20261015 14:10:00")
    args = ap.parse_args()
    start = datetime.strptime(args.start, "%Y%m%d %H:%M:%S") if args.start else None
    end = datetime.strptime(args.end, "%Y%m%d %H:%M:%S") if args.end else None
    os.makedirs(args.out_dir, exist_ok=True)
    summary = analyze(args.log_dir, start, end, args.out_dir)
    print(json.dumps({k: summary[k] for k in summary if k != "file_ranges"}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
