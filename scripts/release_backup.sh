#!/usr/bin/env bash
# ============================================================================
# release_backup.sh — 源码库的本地快照备份 / 回滚 (源码项目用, 如 myframe / rxlogger)
#   放在 <project>/scripts/ 下, 只管本项目。
#   打的是【源码快照】: 排除 build/out/.git/*.a/*.o/logs/data 等编译产物与大件,
#   跟本项目 scripts/pack_<项目>.sh 同口径。
#   这类项目是被各服务链接的库(out/lib/*.a → 服务的 3rdpartylib/), 回滚 = 还原源码后
#   需要重新编译并把库同步回各服务。
#   本项目在 git 里; 这个是 git 之外的快速本地快照/回滚 —— 改坏了不走 git 直接回。
#
# 用法 (任意目录都行):
#   scripts/release_backup.sh backup [tag]            # 源码快照 → <project>/backups/<ts>[_tag]/
#   scripts/release_backup.sh list
#   scripts/release_backup.sh show [ts|latest]        # 看清单(含 git commit)
#   scripts/release_backup.sh rollback [ts]           # 回滚源码: 不给 ts → 最新
#   scripts/release_backup.sh prune [keepN]
#
# 回滚 = 先快照当前源码(pre_rollback, 可逆) → 解包覆盖源码。库不停服务; 之后需重编译。
# ============================================================================
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$(basename "$PROJECT_ROOT")"
BACKUP_ROOT="$PROJECT_ROOT/backups"

# 源码快照排除(编译产物 / 大件 / .git / 自己的备份); 跟 pack_<项目>.sh 一致。
EXCLUDES=(
  --exclude='./.git' --exclude='./.claude' --exclude='./.tmp'
  --exclude='./build' --exclude='./build_*' --exclude='./build-*'
  --exclude='./out' --exclude='./dist' --exclude='./bin'
  --exclude='./backups'
  --exclude='./data' --exclude='./data_*'
  --exclude='./logs' --exclude='./log'
  --exclude='*.o' --exclude='*.a' --exclude='*.so' --exclude='*.so.*'
  --exclude='*.log' --exclude='*.tar.gz' --exclude='*.bak'
  --exclude='*/CMakeFiles' --exclude='*/CMakeCache.txt' --exclude='*/cmake_install.cmake'
  --exclude='*/__pycache__' --exclude='core' --exclude='*.core'
)

err() { echo "[ERROR] $*" >&2; exit 1; }
resolve_ts() {
  local w="$1"
  if [[ "$w" == latest ]]; then ls -1 "$BACKUP_ROOT" 2>/dev/null | grep -vE '_pre_rollback$' | sort | tail -1
  else echo "$w"; fi
}

do_backup() {
  local tag="${1:-}" ts; ts="$(date +%Y%m%d_%H%M%S)"
  local name="$ts"; [[ -n "$tag" ]] && name="${ts}_${tag}"
  local dest="$BACKUP_ROOT/$name"; mkdir -p "$dest"
  tar -C "$PROJECT_ROOT" "${EXCLUDES[@]}" -czf "$dest/$PROJECT.tar.gz" .
  local commit; commit="$(cd "$PROJECT_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo n/a)"
  {
    echo "project: $PROJECT (源码快照)"; echo "backup:  $name"; echo "created: $(date '+%F %T')"
    echo "host:    $(hostname)"; echo "git:     $commit"
    echo "size:    $(du -h "$dest/$PROJECT.tar.gz" | cut -f1)"
    echo "files:   $(tar -tzf "$dest/$PROJECT.tar.gz" | wc -l)"
  } > "$dest/MANIFEST.txt"
  echo "==> [$PROJECT] 源码快照 → $dest/$PROJECT.tar.gz ($(du -h "$dest/$PROJECT.tar.gz" | cut -f1), git $commit)"
  echo "    出问题: scripts/release_backup.sh rollback $name"
}

do_list() {
  [[ -d "$BACKUP_ROOT" ]] || { echo "[$PROJECT] (还没有备份)"; return; }
  echo "[$PROJECT] 源码快照 (在 $BACKUP_ROOT):"
  for d in "$BACKUP_ROOT"/*/; do [[ -d "$d" ]] || continue
    printf "  %-32s %6s  git:%-9s %s\n" "$(basename "$d")" "$(du -sh "$d" 2>/dev/null | cut -f1)" \
      "$(grep -m1 '^git:' "$d/MANIFEST.txt" 2>/dev/null | awk '{print $2}')" \
      "$(grep -m1 '^created:' "$d/MANIFEST.txt" 2>/dev/null | sed 's/created: *//')"
  done
}

do_show() {
  local ts; ts="$(resolve_ts "${1:-latest}")"
  [[ -f "$BACKUP_ROOT/$ts/MANIFEST.txt" ]] || err "找不到备份: $ts (用 list 看)"
  cat "$BACKUP_ROOT/$ts/MANIFEST.txt"
}

do_rollback() {
  local ts=""
  for a in "$@"; do
    case "$a" in
      latest) ts="latest";;
      *) if [[ -d "$BACKUP_ROOT/$a" ]]; then ts="$a"; else err "未知参数: '$a' (用 list 看备份名)"; fi;;
    esac
  done
  [[ -z "$ts" ]] && ts="latest"
  ts="$(resolve_ts "$ts")"
  [[ -n "$ts" && -f "$BACKUP_ROOT/$ts/$PROJECT.tar.gz" ]] || err "没有可回滚的备份 (先 backup, 或 list 看)"
  echo "==> [$PROJECT] 源码回滚到: $ts"
  echo "--> 先快照当前源码 (pre_rollback, 回滚本身可逆)"
  do_backup "pre_rollback" >/dev/null
  tar -xzf "$BACKUP_ROOT/$ts/$PROJECT.tar.gz" -C "$PROJECT_ROOT"
  echo "    [$PROJECT] 源码已还原 (覆盖式: 还原备份内的文件, 不删备份后新增的文件)"
  echo "==> 回滚完成。记得: 重新编译 (scripts/build.sh) + 把 out/lib/*.a 同步回各服务的 3rdpartylib/。"
}

do_prune() {
  local keep="${1:-10}"; [[ -d "$BACKUP_ROOT" ]] || return
  mapfile -t normal < <(ls -1 "$BACKUP_ROOT" | grep -vE '_pre_rollback$' | sort)
  local n=${#normal[@]}; (( n <= keep )) && { echo "[$PROJECT] 普通备份 $n 个 ≤ $keep, 不删"; return; }
  echo "[$PROJECT] 删最旧 $((n-keep)) 个 (留最近 $keep):"
  for ((i=0; i<n-keep; i++)); do echo "  rm ${normal[$i]}"; rm -rf "$BACKUP_ROOT/${normal[$i]}"; done
}

cmd="${1:-}"; shift || true
case "$cmd" in
  backup)   do_backup "$@";;
  list)     do_list;;
  show)     do_show "$@";;
  rollback) do_rollback "$@";;
  prune)    do_prune "$@";;
  *) sed -n '2,20p' "$0"; exit 1;;
esac
