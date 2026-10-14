# macOS module-residency PoC

This isolated PoC tests the loader behavior required before enabling TDLua on
macOS. It has no Lua or TDLib dependency and deliberately refuses to configure
on non-Apple hosts. It is not evidence of macOS support until run on a real
macOS arm64 runner.

On macOS arm64 with Xcode command-line tools:

```sh
cmake -S poc/macos_residency -B build-macos-residency -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-macos-residency --parallel 2
ctest --test-dir build-macos-residency --output-on-failure
```

The pinned test locates the module using `dladdr()`, applies
`RTLD_NOLOAD | RTLD_NODELETE` twice, and balances each extra handle with
`dlclose()`. It releases both original handles, probes the module and its
dependency with `RTLD_NOLOAD`, and checks that static state persists while
their finalizers remain pending. After closing every handle, it compares
`stat()` device/inode identities against dyld's image list. `stat()` follows
symlinks, and the identity check does not open a loader handle. The parent
checks that the worker `thread_local` destructor ran before the zero-handle
marker and that module/dependency finalizers ran once after it.

The unpinned test loads the same images in a separate child process without
calling the pin entry. It releases every handle, records dyld's image-list
result, and reports when each static finalizer ran. A still-mapped unpinned
image makes this control non-discriminating. If image paths cannot be matched
to files by device/inode, the control reports that result as inconclusive.

A missing flag, failed pin, unpinned behavior misreported as proof, lost pinned
image, early pinned finalizer, or missing finalizer is a failure. The unpinned
image may remain resident without failing the experiment; that result means
only that the negative control did not distinguish the pin. Record macOS,
Xcode/Apple Clang, SDK, architecture, and complete CTest output. This PoC does
not prove that retaining the Mach-O image makes an unverified TDLib teardown
safe.

The experimental GitHub Actions workflow runs this PoC on `macos-26`, which is
an Apple Silicon arm64 runner. It triggers only when this workflow or the PoC
changes on `vnext-portability`. The workflow is intentionally not a required
release check.
