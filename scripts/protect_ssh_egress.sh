#!/bin/bash
# 公网 3Mbps。游戏最多 2Mbps，监控最多 0.5Mbps，剩下约 0.5Mbps 给 SSH。
# 监控单独一条队列，不跟游戏包挤在一起。
set -euo pipefail
DEV="${GAMEMESH_EGRESS_DEV:-eth0}"
tc qdisc del dev "$DEV" root 2>/dev/null || true
tc qdisc add dev "$DEV" root handle 1: htb default 20
tc class add dev "$DEV" parent 1: classid 1:1 htb rate 1000mbit ceil 1000mbit
tc class add dev "$DEV" parent 1:1 classid 1:20 htb rate 100mbit ceil 1000mbit prio 0 burst 16kb cburst 16kb
tc class add dev "$DEV" parent 1:1 classid 1:10 htb rate 1600kbit ceil 2000kbit prio 2 burst 16kb cburst 16kb
tc class add dev "$DEV" parent 1:1 classid 1:30 htb rate 400kbit ceil 500kbit prio 1 burst 16kb cburst 16kb
tc qdisc add dev "$DEV" parent 1:20 handle 20: fq_codel
tc qdisc add dev "$DEV" parent 1:10 handle 10: fq_codel
tc qdisc add dev "$DEV" parent 1:30 handle 30: fq_codel
tc filter del dev "$DEV" parent 1: protocol ip prio 1 2>/dev/null || true
tc filter add dev "$DEV" protocol ip parent 1: prio 1 u32 match ip sport 8081 0xffff flowid 1:10
tc filter add dev "$DEV" protocol ip parent 1: prio 1 u32 match ip sport 8083 0xffff flowid 1:10
tc filter add dev "$DEV" protocol ip parent 1: prio 1 u32 match ip sport 8094 0xffff flowid 1:30
echo "egress on $DEV: game 8081+8083 ceil 2Mbit, monitor 8094 ceil 0.5Mbit, ssh keeps the other 0.5Mbit"
