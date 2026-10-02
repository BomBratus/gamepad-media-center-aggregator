# Linux Stremio UI smoke tests

This harness launches the real GMCA C++/Borealis/SDL/mpv app under X11 and
drives a virtual controller through its SDL input path. Its default `smoke`
scenario is fixture-only and does not read local account or credential files.

```sh
./scripts/test-tvbox.sh smoke
./scripts/test-tvbox.sh boot
./scripts/test-tvbox.sh navigation
```

The `smoke` suite includes fresh sign-in, a selected unsupported legacy account
with another valid Stremio account present, and two separate restarts with
different selected Stremio accounts. These check the actual startup view and
active account reported by the runtime harness. The harness also identifies
the real `ConnectionSwitcher` view as `account_switcher` when opened through UI
navigation. It then runs the playback, navigation, search, and error cases.

The runner performs the existing standalone tests, fixture protocol checks,
configures `GMCA_LINUX_TEST_BENCH=ON`, and builds with at most two jobs. It does
not force a CMake generator: an existing build cache is reused, otherwise CMake
uses its default. Set `GMCA_TEST_BUILD_DIR` or pass `--build-dir` to select a
different build tree; pass `--generator` only when you need to choose one
explicitly. It does not clean build outputs or package the app.

One-time Debian packages for the runtime harness:

```sh
sudo apt-get install --no-install-recommends build-essential cmake pkg-config \
  libsdl2-dev libmpv-dev libavformat-dev libcurl4-openssl-dev libdbus-1-dev \
  libwebp-dev libgl-dev libegl-dev libgles-dev scrot ffmpeg xdotool
```

The runner applies the read-only Borealis observer patch only in the local
submodule worktree. Its Unix socket accepts only state, button, axis, and quit
commands. UI state is sampled on the normal main loop; commands cannot open a
screen, set focus, or call a UI navigation method.

## Fixture isolation

Each case uses a fresh private config/cache directory and an ephemeral loopback
Stremio addon/account fixture. A short local video supports playback cases.
Fixture mode exercises the real Stremio request parsing, views, source choice,
and mpv path without contacting streaming providers. Test HTTP requests are
restricted to loopback. Temporary config, history, cache, downloads, and
Stremio watch/progress state are discarded after that run.

Each startup profile uses synthetic IDs and fake fixture tokens. The legacy
case intentionally leaves a Plex profile selected alongside a valid Stremio
profile; startup must show Stremio sign-in rather than choosing another account
implicitly. Multiple-account checks use one selected ID per isolated restart.
Fresh sign-in types only synthetic email/password values through the Linux IME;
the loopback fixture accepts those values and never forwards them to Stremio.

Live account coverage is an explicit opt-in. Supply a private config path and
use `--live`; the runner reads only that file and copies one authenticated
Stremio profile into the isolated runtime:

```sh
GMCA_TEST_LIVE_CONFIG=/private/path/config.json ./scripts/test-tvbox.sh smoke --live
```

Live checks browse Home and Movies without playback or account writes. Without
`--live`, the runner never probes the default GMCA config paths.

## Results and limits

Screenshots, semantic checkpoints, redacted logs, fixture request paths, and
`result.json` are stored in a private `test-results/run-*` directory. A lock
prevents concurrent runs. A stale test process is stopped only if its recorded
PID, executable, and process start time still match the owned test instance.

Linux checks shared behavior. It cannot verify PS4 graphics, input translation,
TLS/memory limits, patched libmpv rendering, package contents, updater
behavior, GoldHEN installation, or PS4 performance. Validate those with the
PS4 Stremio-only package and console.
