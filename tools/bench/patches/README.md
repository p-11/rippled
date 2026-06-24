# Upstream baseline patches

`develop-probes.patch` is the only change applied on top of upstream `develop`
to build the upstream benchmark baseline: the PerfLog `event()` hook, the libxrpl
`BenchProbe` global sink, and the four verification-site probes. These are the
two additive, no-op-safe commits from the benchmark workstream, captured as a
diff against `develop` so they apply cleanly on a fresh checkout.

`tools/bench/run-suite.sh --profile upstream` applies this patch to a detached
`develop` worktree, builds `xrpld`, and runs the benchmark. Regenerate the patch
after rebasing the probe work onto a newer `develop` with:

```
git diff develop <probed-branch> -- \
    include/xrpl/basics/BenchProbe.h include/xrpl/core/PerfLog.h \
    src/libxrpl/basics/BenchProbe.cpp src/libxrpl/protocol/STTx.cpp \
    src/libxrpl/protocol/STValidation.cpp src/libxrpl/server/Manifest.cpp \
    src/test/basics/PerfLog_test.cpp src/xrpld/perflog/detail/PerfLogImp.cpp \
    src/xrpld/perflog/detail/PerfLogImp.h \
    > tools/bench/patches/develop-probes.patch
```
