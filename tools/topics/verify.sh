#!/usr/bin/env bash
#
# Prove the topic branches still add up to what we actually run.
#
#   tools/topics/verify.sh [--ref <ref>] [--build]
#
# Two checks, and the first is the one that matters:
#
#   1. NO CODE LOST.  Rebuild integration from the manifest and compare its
#      tree against a reference -- by default the tag recording the image last
#      flashed. Any difference means a change exists in one place but not the
#      other: either something was committed straight to integration and
#      belongs to no topic (so it would never reach a PR, and the next rebuild
#      would silently delete it), or a topic was carved badly and dropped a
#      hunk on the floor. Both have happened here. This check is the reason
#      this tooling exists; run it after every carve.
#
#   2. EACH TOPIC BUILDS ALONE.  A topic that only compiles on top of another
#      is not a sendable PR. Building only the union hides exactly that, and
#      the breakage surfaces as review feedback instead. Opt in with --build,
#      because it is slow.
#
# Paths flagged `meta` in the manifest are excluded from check 1: the topic
# tooling is expected to exist in integration and not in the flashed image.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

MANIFEST="${MANIFEST:-tools/topics/topics}"
REF="${REF:-flashed/2026-08-21}"
SCRATCH="topics/verify-scratch"
DO_BUILD=0

while [ $# -gt 0 ]; do
  case "$1" in
    --ref)   REF="$2"; shift 2 ;;
    --build) DO_BUILD=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

git rev-parse --verify -q "$REF" >/dev/null \
  || { echo "no such reference: $REF" >&2; exit 1; }

start_branch=$(git rev-parse --abbrev-ref HEAD)
cleanup() { git checkout -q "$start_branch" 2>/dev/null || true
            git branch -qD "$SCRATCH" 2>/dev/null || true; }
trap cleanup EXIT

# Directories owned by `meta` topics are expected to differ from the reference.
excludes=()
while read -r name base flags; do
  case "$name" in ''|'#'*) continue ;; esac
  case "${flags:-}" in *meta*) excludes+=(":(exclude)tools/topics") ;; esac
done < "$MANIFEST"

echo "=============================================================="
echo " check 1: no code lost  (integration vs $REF)"
echo "=============================================================="
tools/topics/rebuild.sh --into "$SCRATCH" >/dev/null
echo "  rebuilt tree : $(git rev-parse --short "$SCRATCH"^{tree})"
echo "  reference    : $(git rev-parse --short "$REF"^{tree})"
echo

if diff_out=$(git diff --stat "$REF" "$SCRATCH" -- . "${excludes[@]}") && [ -z "$diff_out" ]; then
  echo "  PASS -- every hunk we run is owned by a topic."
  lost=0
else
  echo "  FAIL -- integration and the reference disagree:"
  echo
  echo "$diff_out" | sed 's/^/    /'
  echo
  echo "  Inspect with:"
  echo "    git diff $REF $SCRATCH -- . ${excludes[*]}"
  echo
  echo "  A path present in the reference but not in the rebuild was DROPPED by"
  echo "  a carve -- recover it from the reference, do not retype it."
  lost=1
fi

if [ "$DO_BUILD" -eq 1 ]; then
  echo
  echo "=============================================================="
  echo " check 2: each topic builds alone"
  echo "=============================================================="
  failed=()
  while read -r name base flags; do
    case "$name" in ''|'#'*) continue ;; esac
    env=""
    case "${flags:-}" in *env=*) env="${flags##*env=}"; env="${env%%,*}" ;; esac
    if [ -z "$env" ]; then
      printf '  %-26s skipped (no env= in manifest)\n' "$name"
      continue
    fi
    printf '  %-26s building %s ... ' "$name" "$env"
    git checkout -q "$name"
    if pio run -e "$env" >/tmp/topics-build.$$ 2>&1; then
      echo "ok"
    else
      echo "FAILED"
      tail -15 /tmp/topics-build.$$ | sed 's/^/      /'
      failed+=("$name")
    fi
    rm -f /tmp/topics-build.$$
  done < "$MANIFEST"
  echo
  if [ ${#failed[@]} -eq 0 ]; then
    echo "  PASS -- every topic with an env compiles on its own."
  else
    echo "  FAIL -- these do not build standalone: ${failed[*]}"
    lost=1
  fi
fi

echo
exit "$lost"
