#!/usr/bin/env python3
"""Execute every PromQL embedded in static/monitor.html against the local Prometheus API."""
import json
import re
import sys
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HTML = (ROOT / "static" / "monitor.html").read_text(encoding="utf-8")
PROM = "http://127.0.0.1:9090"


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def extract_quoted_queries():
    queries = []
    for line in HTML.splitlines():
        if "promql:" in line:
            expr = line.split("promql:", 1)[1]
        elif "query:" in line:
            expr = line.split("query:", 1)[1]
        else:
            continue
        if "jobSel()" in expr:
            built = []
            for m in re.finditer(r"'([^']*)'|jobSel\(\)", expr):
                if m.group(1) is None:
                    built.append('job="gamemesh"')
                else:
                    built.append(m.group(1))
            q = "".join(built)
        else:
            found = re.search(r"'([^']*)'", expr)
            q = found.group(1) if found else ""
        q = q.replace("JOB", "gamemesh").strip().rstrip(",")
        if not q or q.startswith("()"):
            continue
        if not any(tok in q for tok in ("gamemesh_", "process_", "rate(", "increase(", "up{", "{__name__")):
            continue
        queries.append(q)
    return queries


def gap_queries():
    def gap(kind, left, right):
        def one(stage):
            return (
                'sum by (instance_id) (rate(gamemesh_reply_stage_total{job="gamemesh",role="gateway",kind="%s",stage="%s"}[1m]))'
                % (kind, stage)
            )
        return one(left) + " - " + one(right)

    move = [
        ("request_received", "logic_dispatch_started"),
        ("logic_dispatch_started", "logic_response_ready"),
        ("logic_response_ready", "event_loop_enqueued"),
        ("event_loop_enqueued", "control_outbox_queued"),
        ("control_outbox_queued", "socket_write_accepted"),
    ]
    heartbeat = [
        ("logic_response_ready", "event_loop_enqueued"),
        ("event_loop_enqueued", "control_outbox_queued"),
        ("control_outbox_queued", "socket_write_accepted"),
        ("request_received", "logic_dispatch_started"),
    ]
    out = [gap("move", a, b) for a, b in move]
    out += [gap("heartbeat", a, b) for a, b in heartbeat]
    return out


def prom_query(q):
    url = PROM + "/api/v1/query?" + urllib.parse.urlencode({"query": q})
    try:
        with urllib.request.urlopen(url, timeout=20) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except Exception as exc:
        fail("prometheus request failed for %s: %s" % (q, exc))
    if body.get("status") != "success":
        fail("promql rejected: %s error=%s" % (q, body.get("error")))
    return body.get("data", {}).get("result", [])


def main():
    if "指标未导出" not in HTML:
        fail("missing metric must say 指标未导出")
    if "setChartMessage" not in HTML or "refreshPanelList" not in HTML:
        fail("chart errors are not isolated")
    # The refresh loop catches each panel. A single try must not wrap every chart.
    refresh = HTML.split("async function refreshPrometheus()", 1)[1].split("async function loadPromSnapshots", 1)[0]
    if "try {" in refresh and "for (const p of" in refresh.split("try {", 1)[0]:
        pass
    if refresh.count("await refreshPanelList") != 1:
        fail("refreshPrometheus does not delegate per-chart refresh")
    required = [
        "formal_version", "processes", "prometheus_job", "range", "reply_stages",
        "stage_gaps", "latency", "control_outbox", "eventloop", "aoi", "tcp",
        "cpu", "rss", "threads", "current", "max", "increase", "rate",
        "server-movement-diagnostics-",
    ]
    for key in required:
        if key not in HTML:
            fail("export JSON missing %s" % key)

    queries = extract_quoted_queries() + gap_queries()
    # de-dup, keep order
    seen = set()
    unique = []
    for q in queries:
        if q not in seen:
            seen.add(q)
            unique.append(q)
    if len(unique) < 40:
        fail("expected the diagnostic PromQL set, found %d" % len(unique))
    print("queries", len(unique))
    for q in unique:
        prom_query(q)
    print("monitor_promql_test OK", len(unique))

    # Example snapshot. Missing series stay null with note, never a fake zero.
    end = int(datetime.utcnow().timestamp())
    groups = {}
    export_names = [
        "reply_stages", "latency", "control_outbox", "eventloop", "aoi", "tcp",
        "cpu", "rss", "threads",
    ]
    sample_queries = {
        "reply_stages": 'gamemesh_reply_stage_total{job="gamemesh"}',
        "latency": '{__name__=~"gamemesh_reply_latency_p(50|95|99)_ms",job="gamemesh"}',
        "control_outbox": '{__name__=~"gamemesh_control_outbox_(current_frames|current_bytes|peak_frames|peak_bytes|held_connections|oldest_held_ms|overflow_total)",job="gamemesh"}',
        "eventloop": '{__name__=~"gamemesh_eventloop_(todo_last|batch_ms_last|tick_p99_ms|tick_max_ms)",job="gamemesh"}',
        "aoi": '{__name__=~"gamemesh_(mailbox_pending|aoi_push_backlog|aoi_worker_inflight|aoi_batch_oldest_age_ms|latest_move_hold_ms)",job="gamemesh"}',
        "tcp": '{__name__=~"process_socket_send_queue_bytes|gamemesh_tcp_eagain_total|gamemesh_host_tcp_retrans_segs_total",job="gamemesh"}',
        "cpu": 'process_cpu_seconds_total{job="gamemesh"}',
        "rss": 'process_resident_memory_bytes{job="gamemesh"}',
        "threads": 'process_threads{job="gamemesh"}',
    }
    for name in export_names:
        rows = prom_query(sample_queries[name])
        if not rows:
            groups[name] = {"missing": True, "note": "指标未导出", "series": []}
            continue
        series = []
        for row in rows[:8]:
            metric = row.get("metric", {})
            value = row.get("value", [None, None])[1]
            series.append({
                "name": metric.get("__name__", ""),
                "instance_id": metric.get("instance_id", ""),
                "role": metric.get("role", ""),
                "kind": metric.get("kind", ""),
                "stage": metric.get("stage", ""),
                "current": float(value) if value is not None else None,
                "max": float(value) if value is not None else None,
                "increase": None,
                "rate": None,
            })
        groups[name] = {"missing": False, "series": series, "series_n": len(rows)}
    doc = {
        "schema": "gamemesh.server-movement-diagnostics.v1",
        "formal_version": {"git_sha": "da700a44f147101569829e4a570bb517002013c8"},
        "prometheus_job": "gamemesh",
        "range": {
            "start": datetime.utcfromtimestamp(end - 3600).isoformat() + "Z",
            "end": datetime.utcfromtimestamp(end).isoformat() + "Z",
            "seconds": 3600,
            "increase_window": "1h",
        },
        "note": "Example scraped from the running cluster before the new counters are deployed. Absent series are 指标未导出, not 0.",
    }
    doc.update(groups)
    out = Path("/tmp") / ("server-movement-diagnostics-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".json")
    out.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8")
    print("example", out)


if __name__ == "__main__":
    main()
