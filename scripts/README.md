# Build and development notes

The supported delivery workflow is
[`build-ps4-stremio-only`](../.github/workflows/build-ps4-stremio-only.yml).
It builds the PS4 Stremio-only package and prepares the verified package,
checksum, and update-manifest artifacts consumed by the repository's update
publishing workflow. Publishing is a separate workflow and must only run when
authorized.

## Linux shared-behavior test bench

Linux is not a delivery target. Opt in explicitly when checking behavior shared
with PS4, and cap compilation at two jobs:

```sh
cmake -S . -B build-linux \
  -DGMCA_LINUX_TEST_BENCH=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel 2
```

This does not replace PS4 package or device validation.

## Dependencies

The PS4 workflow applies the local Borealis fixes and installs the pinned PS4
pacbrew packages, including libmpv. Consult that workflow for dependency
versions and toolchain setup.

The isolated Linux runtime smoke harness is run with:

```sh
./scripts/test-tvbox.sh smoke
```

It uses the private fixture profile by default and compiles with at most two
jobs. See [the UI test guide](../tests/ui/README.md) before opting in to live
account coverage.

## PS4 build caches

The Stremio-only build runs on pushes to `dev` and PS4 release branches, on
relevant pull requests, and by manual dispatch. Publishing remains a separate
workflow.

Pinned package downloads are listed in `ps4/dependencies.txt` and cached before
installation. Patched libmpv is cached separately, including its static library,
headers, and pkg-config file. Its exact cache key includes the toolchain image,
architecture, dependencies, patches, and build instructions. No approximate
cache match is used. Source-only changes reuse libmpv; changed build inputs
rebuild it. Pull requests can restore caches but do not save them.

Both fresh and restored libmpv pass the subtitle/shader marker checks. Each run
still compiles GMCA and validates the finished PKG and update manifest. The
first run for a new cache key pays the full dependency build cost; later runs
can reuse it. Actual time savings need a CI run to measure.
