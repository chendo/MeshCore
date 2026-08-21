#!/usr/bin/env bash
#
# Regenerate the integration branch from the topic manifest.
#
#   tools/topics/rebuild.sh [--into <branch>]
#
# Starts at BASE and merges every topic in manifest order. Safe to run as often
# as you like: it is a pure function of the topic branches, so the result is
# reproducible, and rerere replays conflict resolutions you have already made.
#
# It refuses to run with a dirty tree, and it never touches a topic branch --
# only the integration branch, which it recreates from scratch each time.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

MANIFEST="${MANIFEST:-tools/topics/topics}"
BASE="${BASE:-origin/main}"
INTO="integration"

while [ $# -gt 0 ]; do
  case "$1" in
    --into) INTO="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -n "$(git status --porcelain)" ]; then
  echo "refusing to rebuild: working tree is dirty" >&2
  git status --short >&2
  exit 1
fi

# rerere is what makes rebuilding cheap -- without it you re-resolve the same
# MyMesh.cpp conflicts on every run and quickly stop rebuilding at all.
if [ "$(git config --get rerere.enabled || echo false)" != "true" ]; then
  echo "note: rerere is off; enabling it (git config rerere.enabled true)" >&2
  git config rerere.enabled true
  git config rerere.autoupdate true
fi

[ -f "$MANIFEST" ] || { echo "no manifest at $MANIFEST" >&2; exit 1; }

topics=()
while read -r name base flags; do
  case "$name" in ''|'#'*) continue ;; esac
  git rev-parse --verify -q "$name" >/dev/null \
    || { echo "manifest names a branch that does not exist: $name" >&2; exit 1; }
  topics+=("$name")
done < "$MANIFEST"

[ ${#topics[@]} -gt 0 ] || { echo "manifest lists no topics" >&2; exit 1; }

was=$(git rev-parse --abbrev-ref HEAD)
echo "rebuilding '$INTO' from ${#topics[@]} topics on top of $BASE"
git checkout -q -B "$INTO" "$BASE"

for t in "${topics[@]}"; do
  printf '  merging %-26s' "$t"
  if git merge --no-ff --no-edit -q "$t" >/tmp/topics-merge.$$ 2>&1; then
    echo "ok"
  else
    echo "CONFLICT"
    echo
    echo "Conflicted paths:" >&2
    git diff --name-only --diff-filter=U >&2
    echo >&2
    echo "Resolve, 'git add' them, then 'git merge --continue' and re-run." >&2
    echo "rerere will remember the resolution for next time." >&2
    rm -f /tmp/topics-merge.$$
    exit 1
  fi
  rm -f /tmp/topics-merge.$$
done

echo
echo "'$INTO' rebuilt: $(git rev-parse --short HEAD)  tree $(git rev-parse --short HEAD^{tree})"
echo "(was on '$was'; still checked out on '$INTO')"
