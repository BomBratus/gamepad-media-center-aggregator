# GMCA — PS4 Stremio client

GMCA is a native, controller-first media client. **This development branch is
specialized for PS4 and Stremio.** It builds a PS4 package with Stremio as its
connection flow. Linux is available only as an explicitly selected test bench
for shared behavior; it is not a delivery target.

The repository retains the project's earlier multi-platform and multi-backend
history. The current development and validation path for this branch is PS4
with Stremio.

## PS4 package

The PS4 Stremio-only workflow builds the package and uploads it as a run
artifact:

[`build-ps4-stremio-only`](.github/workflows/build-ps4-stremio-only.yml)

The package keeps the GMCA title identity (`GMCA00000`) and PS4 package
revision `00.82`. Workflow artifacts include the versioned package, a stable
download name, SHA-256 checksums, and the update manifest consumed by the
in-app updater. The updater verifies and downloads the package to `/data/pkg`,
then prompts for manual installation through GoldHEN Package Installer. A
separate workflow publishes the rolling feed after a validated PR build.

## Build and test

The workflow is the supported PS4 build path. It uses the PS4 OpenOrbis
toolchain and the pinned pacbrew dependencies, including libmpv. For shared
behavior checks on Linux, opt in explicitly and limit the build to two jobs:

```sh
cmake -S . -B build-linux \
  -DGMCA_LINUX_TEST_BENCH=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel 2
```

The Linux test bench does not verify PS4 graphics, input, networking, playback,
or package behavior. Use the actual PS4 Stremio-only workflow and device for
those checks.

## Stremio and playback controls

The PS4 build starts with the Stremio connection flow. Sign in to the Stremio
account that has the desired catalogues and add-ons configured. Stream
availability and playback depend on the selected Stremio add-ons and source.

| PS4 controller | Keyboard on Linux test bench | Playback action |
|:---:|:---:|---|
| Cross | `space` | Play / pause |
| Circle | `esc` | Stop |
| Triangle | `o` | Toggle OSD |
| Square | `f4` | Open menu |
| R1 / L1 | `]` / `[` | Seek forward / back |
| Share | `f1` | Video profile |
| R3 | `f2` | Video quality |
| L3 | `f3` | Playback speed |

## Development notes

The app is C++17 and uses [Borealis](https://github.com/natinusala/borealis)
for its interface and [mpv](https://mpv.io) for playback. Apply the project's
Borealis patch before a local build, as the PS4 workflow does:

```sh
git -C library/borealis apply ../../scripts/patches/borealis-fixes.patch
```

The current workflow configuration is the reference for PS4 compiler options,
dependencies, package identity, version, and artifact naming. Keep those
contracts aligned when changing the build.

## Acknowledgements

GMCA retains work and inspiration from the homebrew and open-source community:

- [@dragonflylee](https://github.com/dragonflylee) for
  [Switchfin](https://github.com/dragonflylee/switchfin), the project GMCA
  forked from.
- [@xfangfang](https://github.com/xfangfang) for
  [wiliwili](https://github.com/xfangfang/wiliwili).
- [@natinusala](https://github.com/natinusala) and XITRIX for
  [Borealis](https://github.com/natinusala/borealis).
- [@devkitPro](https://github.com/devkitPro) and switchbrew for
  [libnx](https://github.com/switchbrew/libnx).
- [@proconsule](https://github.com/proconsule) for
  [nxmp](https://github.com/proconsule/nxmp).
- [@averne](https://github.com/averne) for FFmpeg and mpv hardware-acceleration
  work.

The [changelog](resources/CHANGELOG.md) remains the historical record of earlier
platforms, backends, and contributors.
