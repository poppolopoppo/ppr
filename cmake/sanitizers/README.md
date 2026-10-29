# Linux sanitizer checks

ASan and LSan remain enabled for engine allocations. Linux ASan CTest entries
and the `run-game-clang-dev` build preset use `nvidia.lsan.supp` for two NVIDIA
driver libraries only. Other sanitizer options are retained; this policy selects
the suppression file instead of any earlier `suppressions` option. UBSan CTest
checks stop on the first diagnostic rather than succeeding after printing it.

## NVIDIA allocations

The isolated UI pixel-readback test on NVIDIA 615.78.08 reports 549 bytes in
12 allocations, all inside `libnvidia-glcore.so` and `libnvidia-glsi.so` after
the RHI device and instance have been destroyed. The same test is leak-free
with Mesa llvmpipe. These are external driver reports, not engine fixes.

`VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1` keeps driver code and static
storage mapped until process exit. This preserves stack symbols and prevents
unloaded globals from becoming false unreachable roots. Only the two named
NVIDIA libraries are suppressed; there are no engine, allocator, unknown-module,
DBus, or general Vulkan suppressions. LSan prints the matched allocation counts
and bytes. A separate intentional application allocation still fails leak
detection with these suppressions active.

For a direct executable invocation with the same policy:

```sh
VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1 \
LSAN_OPTIONS="${LSAN_OPTIONS:+${LSAN_OPTIONS}:}suppressions=$PWD/cmake/sanitizers/nvidia.lsan.supp" \
out/build/clang-dev/lib/engine/tests/app/engine.tests.app \
  --run-test app/imgui_routing/detached_colors_decode_only_for_srgb_targets_before_interpolation
```

Run a direct executable without these environment settings to inspect the
unsuppressed baseline. Reassess the suppressions when updating the NVIDIA driver;
new modules or new engine allocations are not covered by this policy.

## Asset GPU allocation stacks

Linux ASan uses the slow allocation-stack unwinder with a 20-frame context only
for `engine.tests.asset.gpu`. The default fast unwinder can stop inside DBus,
omitting the NVIDIA caller responsible for a reported allocation. Retaining that
caller lets the existing NVIDIA-only suppression match; no DBus or general
suppression is added, and engine allocation checks remain active. CTest appends
`fast_unwind_on_malloc=0:malloc_context_size=20` to `ASAN_OPTIONS`, preserving
the other existing options.

The full asset GPU suite passed in 299.38 seconds with these stack settings on
NVIDIA 615.78.08 in `clang-dev`. Earlier approximately 23-second runs failed
rendering assertions and are not equivalent timing baselines. Its
450-second timeout and all assertions remain unchanged. Other CTest entries
retain their existing unwinder settings; this change adds no library preload.

## STB resize

`PatchStbResize.cmake` checks the pinned header hash and builds a private copy.
Constant-sized `memmove` operations replace integer-pointer copies of float
coefficients, preserving overlapping packing and four-byte alignment. SIMD
resampling and all sanitizer instrumentation remain enabled. CPM sources stay
unchanged.
