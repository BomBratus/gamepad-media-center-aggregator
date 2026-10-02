# Runtime validation

Preserve user changes. Start from current remote dev; inspect HEAD, open PRs,
relevant branches and recent workflow results before changing code. Use an
isolated worktree when the existing checkout has user edits.

For Debian/Xorg validation run `./scripts/test-tvbox.sh smoke --sync`.
Focused scenarios and environment/setup instructions are in `tests/ui/README.md`.
Use Ninja, incremental builds and at most two compiler jobs on the TV box.
Never delete the build directory to perform an ordinary test iteration.

Tests must preserve the user's real configuration and account state. Use the
isolated fixture profile by default. Live tests must remain read-only and must
not export tokens or signed URLs into logs, screenshots or tracked files.

Shared changes affecting PS4 require the real `build PS4 Stremio-only` CI
workflow, including source/Borealis/libmpv patches and PKG validation. A Linux
PASS does not validate PS4-specific rendering, player or package behavior.
Use remote CI for OpenOrbis builds; do not cross-build on the two-core box.
Use the native luna agent for PS4 operation and test execution.

# PS4 safety

Never update the user's PS4 firmware. The console at `192.168.1.149` runs
firmware 9.00 and must remain on that version. Remote Play is started only
when requested. Do not deliver/install packages or modify remote resources
without task authorization.
