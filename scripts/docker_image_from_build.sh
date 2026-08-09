#!/usr/bin/env bash
# 将 build/test 角色二进制打包为 gamemesh:local（供 compose 使用）
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN="${ROOT}/build/test"
STAGE="${ROOT}/build/docker_stage"
TAG="${GAMEMESH_IMAGE_TAG:-gamemesh:local}"

for b in gateway session gamelogic world gamedb; do
  if [[ ! -x "$BIN/$b" ]]; then
    echo "ERROR missing $BIN/$b (build full with ENABLE_BRPC=ON first)" >&2
    exit 1
  fi
done

rm -rf "$STAGE"
mkdir -p "$STAGE/lib"
for b in gateway session gamelogic world gamedb; do
  cp -a "$BIN/$b" "$STAGE/$b"
  strip --strip-unneeded "$STAGE/$b" 2>/dev/null || true
done

# 收集动态库（跳过 vdso / ld-linux；已在基础镜像的 libc 等仍复制一份以保证兼容）
collect_libs() {
  local bin="$1"
  ldd "$bin" | awk '/=>/ {print $3} /^\// {print $1}' | while read -r so; do
    [[ -z "$so" || "$so" == "not" ]] && continue
    [[ -f "$so" ]] || continue
    case "$so" in
      *ld-linux*|*linux-vdso*) continue ;;
    esac
    local base
    base="$(basename "$so")"
    if [[ ! -f "$STAGE/lib/$base" ]]; then
      cp -aL "$so" "$STAGE/lib/$base" 2>/dev/null || cp -a "$so" "$STAGE/lib/$base"
    fi
  done
}
for b in gateway session gamelogic world gamedb; do
  collect_libs "$STAGE/$b"
done

cp -a "$ROOT/deploy/docker-entrypoint.sh" "$STAGE/"
cp -a "$ROOT/config" "$STAGE/config"
cp -a "$ROOT/deploy/Dockerfile.runtime" "$STAGE/Dockerfile"

docker build -t "$TAG" "$STAGE"
rm -rf "$STAGE"
echo "docker_image_from_build.sh PASS tag=$TAG"
