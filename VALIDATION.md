# PS4 / Stremio specialization — local validation

Work was performed in the isolated `/home/michele/gmca-ps4-only` worktree,
based on `dev` at `913bbd2bef8ef126f745429c3295ecc7191607f7`. The original
working tree and its user changes were left untouched. No commits or remote
writes were made during that local validation phase.

The maintained source includes the PS4 application patches and search
transformation, plus the committed PS4 00.82 Stremio playback, history,
subtitle, rendering and Update behavior from
`895eafc8eefa3ce5657bedfded98cfb0a99058be`. No uncommitted release-worktree
source was copied. Required Borealis and PS4 libmpv patches remain.

## Passed checks

- All 16 standalone media/account/Stremio unit tests; the final account guard
  received one focused recheck after its change.
- Nine fixture/profile/harness unit tests.
- Incremental Linux SDL2 test-bench build with at most two compiler jobs.
- All 17 isolated fixture runtime scenarios: fresh setup and sign-in, legacy
  selected account, both selected Stremio accounts and switching, navigation,
  library, Continue Watching, source selection, subtitles, next episode,
  downloads and offline playback, resume, movies, watched state, search, and
  loading/error recovery.
- Removed platform selections fail configuration; Linux without the explicit
  bench option fails. The default selects PS4.
- Application/resource audit has no removed service references or unsupported
  platform branches. Remaining upstream library platform support is retained.
- Final staged and unstaged diff whitespace checks.

The runtime suite reused the successfully linked application. Only failed
harness cases were retried; passed cases were not repeated. The offline test
queued an actual fixture download through the source menu, verified the
completed file/index/cached source, stopped the server, and played locally.

[Consolidated runtime results](test-results/latest-smoke-summary.json) record
all observed passes. The old runner pruned earlier raw run directories during
retries; that pruning is removed, and the summary identifies which raw
artifacts remain. Earlier passes are retained as session observations.

## PS4 validation still required

At the end of local validation, no PS4 workflow, package validation, console
test, publication, or delivery had been performed. OpenOrbis is unavailable locally, and remote actions and
console delivery were outside this task's authorization. Linux results do
not establish PS4 readiness. The retained PS4 workflow must verify the
installed identity, package revision, patched dependencies and Update
artifacts before authorized delivery. Published-manifest verification belongs
to an authorized Update delivery. Console firmware was not changed.
