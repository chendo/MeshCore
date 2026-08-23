# `test_companion_node_prefs` reports PASSED while testing nothing

**Severity:** low. No incorrect behaviour ships. But a test directory that is
green and empty is worse than no directory, because it reads as coverage.

## What happens

`test/test_companion_node_prefs/test_companion_node_prefs.cpp` holds exactly one
`TEST`, and it is wrapped in `#if 0` (line 53 to line 78), added by
`e78bff00 * unit test no longer valid`:

```cpp
#if 0
// Re-enable test once we can SET fem_ values in companion
TEST(CompanionNodePrefs, RxGainSettingsRoundTripIndependently) {
  ...
}
#endif
```

Disabling the test was reasonable — it asserts behaviour the companion cannot do
yet. The problem is what the disabling *looks like* from outside.

Nothing registers, so `RUN_ALL_TESTS()` returns 0, and PlatformIO reports:

```
--------- native:test_companion_node_prefs [PASSED] Took 0.43 seconds ---------
================== 0 test cases: 0 succeeded in 00:00:00.922 ==================
```

A green `[PASSED]` next to `0 test cases`. In the aggregate run the directory
simply contributes nothing, and the count is indistinguishable from a directory
that does not exist. Nothing in CI says a test is waiting to be restored.

The general form is worth stating: **an empty gtest binary always passes.** Any
future `#if 0` around the last test in a directory produces a permanently green
result, and so does an `#ifdef` whose macro is never defined in the test env.

## Fix

Use gtest's own mechanism. Rename rather than compile out:

```cpp
// Re-enable once the companion can SET fem_ values.
TEST(CompanionNodePrefs, DISABLED_RxGainSettingsRoundTripIndependently) {
```

`DISABLED_` keeps the test compiled, so it cannot rot against the headers it
uses, and gtest prints `YOU HAVE 1 DISABLED TEST` on every run. The gap stays
visible, and `--gtest_also_run_disabled_tests` can exercise it on demand.

Two lines: delete `#if 0` and `#endif`, add the prefix.

## Optional follow-up

A directory whose binary registers zero tests could be treated as a failure
rather than a pass. That is a PlatformIO-side behaviour rather than a MeshCore
one, so it is out of scope here — but it is the reason this went unnoticed.
