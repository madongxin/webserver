#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""parse_logs 边界样本：空文件、缺时间、正常行、跨服务。"""
from __future__ import print_function

import os
import tempfile

from parse_logs import analyze, parse_ts, classify
from datetime import datetime


def main():
    fails = 0

    def expect(c, msg):
        global fails
        if not c:
            print("FAIL:", msg)
            fails += 1
        else:
            print("PASS:", msg)

    expect(parse_ts("20261015 13:39:01.123 INFO x") is not None, "parse_ts normal")
    expect(parse_ts("no timestamp here") is None, "parse_ts missing")
    expect(classify("ClientHello ok conn#1", "gateway")[0] == "hello_ok", "hello")
    expect(classify("unrelated", "gateway") is None, "unrelated")

    d = tempfile.mkdtemp()
    open(os.path.join(d, "gw0.log"), "w").close()
    with open(os.path.join(d, "gw1.log"), "w") as f:
        f.write(
            '{"timestamp":"20261015 13:39:01.","level":"INFO","msg":"20261015 13:39:01.000000Z 1 INFO  ClientHello ok conn#1 platform=x - GameTcpGateway.cpp:1\\n"}\n'
        )
        f.write(
            "20261015 13:39:02.000000Z 1 INFO  Gateway login orchestrated player=100 logic=gl-0 conn=1 - GatewayLoginOrchestrator.cpp:283\n"
        )
        f.write("line without time Gateway login orchestrated player=101\n")
    out = tempfile.mkdtemp()
    s = analyze(
        d,
        datetime(2026, 10, 15, 13, 35, 0),
        datetime(2026, 10, 15, 14, 10, 0),
        out,
    )
    expect(s["unique_login"] == 1, "one login in window")
    expect(os.path.isfile(os.path.join(out, "timeline_10s.csv")), "timeline written")
    expect(os.path.isfile(os.path.join(out, "summary.json")), "summary written")
    if fails:
        print("FAILED", fails)
        raise SystemExit(1)
    print("OK test_parse_logs")


if __name__ == "__main__":
    main()
