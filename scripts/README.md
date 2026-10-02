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
