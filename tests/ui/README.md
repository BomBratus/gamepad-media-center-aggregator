# Linux TV runtime tests

Run from the GMCA checkout on Debian/Xorg:

```sh
./scripts/test-tvbox.sh smoke --sync
./scripts/test-tvbox.sh boot
./scripts/test-tvbox.sh navigation
./scripts/test-tvbox.sh movies
./scripts/test-tvbox.sh series
./scripts/test-tvbox.sh continue-watching
./scripts/test-tvbox.sh resume
./scripts/test-tvbox.sh source-picker
./scripts/test-tvbox.sh watched
./scripts/test-tvbox.sh search
./scripts/test-tvbox.sh error-loading
```

`--sync` fetches origin refs. It never resets, merges, commits or pushes working
changes. Check the current dev and PRs before making code changes. Without this
flag the runner tests exactly the local candidate, including uncommitted changes.
`--runtime-only` skips unit tests and build for debugging an already built candidate;
it is not a complete validation gate.

The full command runs existing standalone unit tests, configures Ninja, builds
incrementally with at most two jobs, then launches the real GMCA C++/Borealis/SDL/
libmpv application on `DISPLAY=:0`, with `XAUTHORITY=/home/michele/.Xauthority`.
These can be overridden in the environment. No Debian packaging step or build
cleanup occurs. ccache is selected when available. Build temporaries go onto the
build filesystem instead of Debian's small `/tmp` tmpfs.

One-time Debian dependencies (runtime tests never need sudo):

```sh
sudo apt-get install --no-install-recommends build-essential cmake ninja-build \
  pkg-config libsdl2-dev libmpv-dev libavformat-dev libcurl4-openssl-dev \
  libdbus-1-dev libwebp-dev libgl-dev libegl-dev libgles-dev scrot ccache
```

SDL >= 2.24 is needed for `SDL_JoystickAttachVirtualEx`. A game-controller virtual
joystick is created **inside GMCA**, after Borealis's SDL input manager is ready.
Every required button is sampled through Borealis's input manager at startup.
Scenario inputs use SDL virtual button state and axes; they never call a UI
click/focus/navigation method, use xdotool, or require a physical gamepad/uinput.
The socket cannot open screens or set focus. Its only commands are state, button,
axis and quit. UI introspection runs on the main thread after each normal loop.

The test build requires Linux + `PLATFORM_DESKTOP` + SDL. The normal desktop build
keeps its defaults (GLFW, multiple backends, no harness). PS4 cannot enable the
harness. The small Borealis observer patch adds read-only inline accessors guarded
by `GMCA_TEST_HARNESS`; there is no production observer thread or socket.

## Isolation and artifacts

Each scenario creates a fresh private temporary `XDG_CONFIG_HOME` and `XDG_CACHE_HOME`.
Config migration therefore sees only this isolated directory, never the real
GMCA/pleNx/Switchlex folders. Config, search history, image cache, downloads,
`stremio-watched.json`, `stremio-progress.json`, and `stremio-playback.json` all use
the isolated profile. Cleanup removes only that run's temporary directory.

Fixtures use an ephemeral loopback HTTP addon and fake account datastore. They
exercise real Stremio parsing, async requests, history, views, source resolution,
and libmpv, with a generated local test video. Fixture builds reject external
HTTP requests. They do not query real streaming providers. The HTTP test hook is
compiled out of production. The account/datastore fixture is not a second UI or
a mock media backend.

A process ownership record lets the next run terminate a stale **test** instance
only after matching executable and process start time. A runner lock prevents
concurrent launches/builds. Production GMCA instances are never killed by name.

`test-results/run-<timestamp>-<scenario>/` contains PNG screenshots from the real X
root window, semantic JSON checkpoints, sanitized `gmca.log`, unit/build logs,
request paths, and `result.json`. Smoke checkpoints/logs are grouped in one subdirectory per scenario; the root
result lists every completed case and the exact failed step. Five recent runs are retained. Logs are bounded;
fixture snapshots include labels, while live snapshots include only IDs/classes.
Files are created with private permissions. A failed assertion records its precise step and the last semantic state.

## Live integration

Smoke automatically adds the read-only live case when an authenticated local
config is available; otherwise its result records an explicit SKIP. Use
`smoke --fixture-only` for deterministic offline checks. An expired account
remains a live FAIL rather than being silently replaced with fixture results.

```sh
GMCA_TEST_LIVE_CONFIG=/private/path/config.json ./scripts/test-tvbox.sh live
```

The default candidate is `~/.config/GMCA/config.json`, with an optional private
local copy at `~/.cache/gmca-tvtest-live/config.json` as fallback. The runner copies only one
authenticated Stremio server/user into its private profile. It does not copy
history, downloads, caches, other backend credentials, or account progress files.
An absent authenticated config makes the live scenario fail explicitly; it does
not silently replace live validation with fixtures. The live scenario browses home
and Movies without playback or watched changes. The test HTTP guard permits the
account read APIs and rejects account writes. Tokens and signed URLs are removed
before stdout/stderr is persisted, and are never placed into result artifacts.
Live screenshots are allowlisted to Home and Movies catalog views without an
open dialog. Failed live boots outside those views suppress screenshots. No
login/settings/token form or live progress datastore is exported.

## PS4 equivalence and limits

The Stremio-only CMake profile is shared and defaults OFF. Episode source-picker
layout, controller focus, resume reuse and grid restoration were moved from the
PS4 source patches into shared C++ for this profile. Italian-audio badges use the
existing classifier; subtitles-only ITA must never count as Italian audio.
PS4 package metadata and the safety source ordering remain in the PS4 patches.
Search debounce and stale-response guards also compile from shared C++ in the
Stremio-only profile; the PS4 AWK step recognizes this and passes current source
through unchanged. It retains its legacy transform for older source trees. The
normal multi-backend search retains its prior behavior. This profile still must
not be described as an exact effective PS4 source tree.

Changes to shared code must pass the real **build PS4 Stremio-only** workflow:
safe-source patch, followup patch, search transform, Borealis patch, patched mpv
0.36, OpenOrbis compile, PKG validation and artifact upload. Local patch application
checks are useful but are not an OpenOrbis build. Linux uses mpv 0.40 and cannot
validate Piglet, PS4 rendering/FBO/shaders, custom libmpv or subtitle rendering,
PS4 memory/TLS, PKG updater/lifecycle, GoldHEN or installation. Those still need
OpenOrbis CI and PS4 hardware.

RSS peak and boot/navigation durations describe regressions on this box only.
They do not predict PS4 performance.
