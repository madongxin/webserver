#!/usr/bin/env bash
# E2E 进程清单与健康检查公共库（source 使用）
# inventory.tsv 列：role instance_id pid rpc_addr http_addr game_addr
# shellcheck shell=bash

e2e_inv_file() {
  echo "${GAMEMESH_RUN_DIR:-}/inventory.tsv"
}

e2e_inv_init() {
  local f
  f="$(e2e_inv_file)"
  : >"$f"
  echo "# role instance_id pid rpc_addr http_addr game_addr" >>"$f"
}

e2e_inv_append() {
  local role="$1" iid="$2" pid="$3" rpc="${4:--}" http="${5:--}" game="${6:--}"
  local f
  f="$(e2e_inv_file)"
  printf '%s %s %s %s %s %s\n' "$role" "$iid" "$pid" "$rpc" "$http" "$game" >>"$f"
}

# 按 role+instance_id 取字段；多行时取最后一条（重启后追加）
e2e_inv_field() {
  local role="$1" iid="$2" field="$3"  # field: pid|rpc|http|game
  local f line r i p rpc http game last=""
  f="$(e2e_inv_file)"
  [[ -f "$f" ]] || return 1
  while read -r line; do
    [[ "$line" =~ ^# ]] && continue
    [[ -z "$line" ]] && continue
    # shellcheck disable=SC2086
    set -- $line
    r="${1:-}" i="${2:-}" p="${3:-}" rpc="${4:--}" http="${5:--}" game="${6:--}"
    if [[ "$r" == "$role" && "$i" == "$iid" ]]; then
      case "$field" in
        pid) last="$p" ;;
        rpc) last="$rpc" ;;
        http) last="$http" ;;
        game) last="$game" ;;
        *) return 2 ;;
      esac
    fi
  done <"$f"
  [[ -n "$last" ]] || return 1
  echo "$last"
}

e2e_pid_of() { e2e_inv_field "$1" "$2" pid; }
e2e_http_of() { e2e_inv_field "$1" "$2" http; }
e2e_rpc_of() { e2e_inv_field "$1" "$2" rpc; }

# 替换某 role/iid 行（删旧后 append）；用于 failpoint 重启
e2e_inv_replace() {
  local role="$1" iid="$2" pid="$3" rpc="${4:--}" http="${5:--}" game="${6:--}"
  local f tmp
  f="$(e2e_inv_file)"
  tmp="$(mktemp)"
  if [[ -f "$f" ]]; then
    while read -r line; do
      [[ "$line" =~ ^# ]] && { echo "$line"; continue; }
      [[ -z "$line" ]] && continue
      # shellcheck disable=SC2086
      set -- $line
      if [[ "${1:-}" == "$role" && "${2:-}" == "$iid" ]]; then
        continue
      fi
      echo "$line"
    done <"$f" >"$tmp"
    mv "$tmp" "$f"
  else
    e2e_inv_init
  fi
  e2e_inv_append "$role" "$iid" "$pid" "$rpc" "$http" "$game"
}

# 删除某 role/iid 行（实验实例退出后清理）
e2e_inv_remove() {
  local role="$1" iid="$2"
  local f tmp
  f="$(e2e_inv_file)"
  tmp="$(mktemp)"
  [[ -f "$f" ]] || return 0
  while read -r line; do
    [[ "$line" =~ ^# ]] && { echo "$line"; continue; }
    [[ -z "$line" ]] && continue
    # shellcheck disable=SC2086
    set -- $line
    if [[ "${1:-}" == "$role" && "${2:-}" == "$iid" ]]; then
      continue
    fi
    echo "$line"
  done <"$f" >"$tmp"
  mv "$tmp" "$f"
}

# 关键角色进程仍存活；缺 inventory 时回退检查 pids 非空且至少 8 行进程存活
# 实验实例 gl-2：未开 DYNAMIC_SCALE 时忽略死进程，避免污染健康检查
e2e_cluster_healthy() {
  local run="${GAMEMESH_RUN_DIR:-}"
  local f host="${E2E_HOST:-127.0.0.1}"
  [[ -n "$run" && -f "$run/pids" ]] || return 1
  f="$run/inventory.tsv"
  if [[ -f "$f" ]]; then
    local need_dead=0
    local line r i p http
    while read -r line; do
      [[ "$line" =~ ^# ]] && continue
      [[ -z "$line" ]] && continue
      # shellcheck disable=SC2086
      set -- $line
      r="${1:-}" i="${2:-}" p="${3:-}" http="${5:--}"
      case "$r" in
        session|gamedb|gamelogic|gateway|world) ;;
        *) continue ;;
      esac
      if [[ "$r" == "gamelogic" && "$i" == "gl-2" && "${GAMEMESH_EXPERIMENTAL_DYNAMIC_SCALE:-0}" != "1" ]]; then
        continue
      fi
      if [[ -z "$p" ]] || ! kill -0 "$p" 2>/dev/null; then
        echo "UNHEALTHY: $r/$i pid=$p dead" >&2
        need_dead=1
        continue
      fi
      if [[ "$http" != "-" && -n "$http" ]]; then
        if ! curl -fsS -m 2 "http://${host}:${http}/health/live" >/dev/null 2>&1; then
          echo "UNHEALTHY: $r/$i http=$http not live" >&2
          need_dead=1
        fi
      fi
    done <"$f"
    [[ "$need_dead" -eq 0 ]]
    return
  fi
  # 兼容旧 pids：逐行检查存活
  local alive=0 total=0 pid
  while read -r pid; do
    [[ -z "$pid" ]] && continue
    total=$((total + 1))
    kill -0 "$pid" 2>/dev/null && alive=$((alive + 1))
  done <"$run/pids"
  (( total >= 8 && alive == total ))
}

# 不健康则 stop+start；需要调用方已设置端口环境
e2e_ensure_cluster() {
  local root="${1:-}"
  export GAMEMESH_RUN_DIR="${GAMEMESH_RUN_DIR:-$root/run/e2e}"
  local i
  for i in 1 2 3 4 5 6; do
    if e2e_cluster_healthy; then
      echo "e2e cluster healthy at $GAMEMESH_RUN_DIR"
      return 0
    fi
    sleep 1
  done
  echo "e2e cluster unhealthy or missing — restarting..."
  "$root/scripts/stop_e2e_cluster.sh" 2>/dev/null || true
  sleep 1
  rm -f "$GAMEMESH_RUN_DIR/pids" "$GAMEMESH_RUN_DIR/inventory.tsv"
  "$root/scripts/run_e2e_cluster.sh"
  sleep 3
  # shellcheck disable=SC1090
  [[ -f "$GAMEMESH_RUN_DIR/E2E_PORTS.env" ]] && source "$GAMEMESH_RUN_DIR/E2E_PORTS.env"
  for i in 1 2 3 4 5 6 7 8 9 10; do
    if e2e_cluster_healthy; then
      return 0
    fi
    sleep 1
  done
  echo "ERROR: cluster still unhealthy after restart" >&2
  return 1
}
