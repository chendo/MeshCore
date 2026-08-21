# Topic branches

This fork carries a lot of change against upstream MeshCore. The point of this
directory is to let those changes go upstream as small, individually reviewable
PRs while we keep running the union of all of them.

## The one rule

**`integration` is a build output, not a place to work.**

Topic branches are the source of truth. `integration` — the branch you flash —
is regenerated from them by `rebuild.sh` and is discarded on every rebuild. A
change committed straight to `integration` belongs to no topic, will never
appear in a PR, and disappears the next time anyone rebuilds.

That is not a hypothetical. Two commits were once hand-split to separate their
BLE halves from their clock halves; the BLE halves reached the public branch and
the clock halves reached nothing at all. The feature was missing from every
branch for days, and it was noticed by a human wondering where a feature went,
not by any tool. `verify.sh` is the tool that would have caught it in seconds.

## Daily use

```sh
tools/topics/rebuild.sh          # regenerate integration from the manifest
tools/topics/verify.sh           # prove nothing was lost  <- run after every carve
tools/topics/verify.sh --build   # also prove each topic compiles alone (slow)
```

Work on a topic branch, then rebuild:

```sh
git checkout t/clock-convergence
# ...edit, commit...
tools/topics/rebuild.sh && tools/topics/verify.sh
```

Conflicts during rebuild are normal — nearly everything touches
`examples/simple_repeater/MyMesh.cpp`. `rerere` is enabled, so you resolve each
one once and subsequent rebuilds replay it.

## Carving a topic out of `t/stack`

`t/stack` is the not-yet-separated remainder: 64 commits spanning roughly ten
areas. To prepare a PR, lift one area onto its own branch:

```sh
git checkout -B t/my-area origin/main
git cherry-pick -x <the commits for that area>

# drop the same commits from t/stack
git checkout t/stack
GIT_SEQUENCE_EDITOR='...delete those picks...' git rebase -i origin/main

# add t/my-area to the manifest, above t/stack, then:
tools/topics/rebuild.sh && tools/topics/verify.sh
```

If `verify.sh` passes, the carve moved code without changing what we run. **If
it fails, recover the missing hunks from the reference tag — do not retype
them.** Retyping is how subtle divergence gets introduced.

Much of `t/stack`'s history is iteration on itself — fixes to earlier commits in
the same stack. Squash those into what they fix before offering any of it
upstream. 64 commits is not 64 PRs; it is about ten.

## The reference tag

`verify.sh` compares against `flashed/2026-08-21`, a tag recording the tree of
the image actually running on the repeaters. Re-tag it when you flash something
new and have confirmed it works:

```sh
git tag -f flashed/$(date +%F) integration
tools/topics/verify.sh --ref flashed/$(date +%F)
```

Keeping the old tags costs nothing and gives you a known-good tree to diff
against when a node starts misbehaving.

## Safety net

`backup/2026-08-21/*` tags record every branch as it stood before this tooling
was introduced. Nothing in this workflow deletes them.
