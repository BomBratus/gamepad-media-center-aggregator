# Stremio / PS4 audit

Baseline: `dev` at `1514f2aab21c9d78c97b64b6dd36a9af69252a99`.
The PS4 source is the base tree **plus**, in order, `ps4-safe-sources.patch`,
`ps4-ui-followups.patch`, the search AWK transform, and Borealis patches.
The package version is resolved from the first patch, now `00.80`.

| Area | Confirmed cause | Focused change / logic tests |
| --- | --- | --- |
| Continue Watching | Series with a completed checkpoint were removed by the positive-offset filter. | Resolve an episode from metadata even at zero offset; keep a series until no valid continuation exists. `test_episode_continuation`. |
| Episode continuation | CW, Next Up and the player selected episodes independently. | Shared partial/completed/forward selector, watched skips, season transitions and no restart of finished series. Same 90% completion threshold. Specials and existing opaque codecs retained. |
| Up Next/autoplay | Player used adjacent array index, including watched episodes. | One next-target callback drives player button, prompt and EOF autoplay. No stream prefetch. `getAllEpisodes` already ignored its stream flag in the baseline; that specific prefetch concern was not confirmed. |
| Persistence | Progress and watched JSON still performed caller-side full-file read/write/rename. | Queue captured mutations and completion clear on the existing writer; cached saved-source lookup does not read disk. Navigation reads may wait for durability off the playback/UI path. Stop, pause, seek and EOF enqueue state; shutdown/restart flush and destructor drain. |
| Coalescing | Every queued save was a separate full-file transaction. | Replace only adjacent pending snapshots for the same scope/item; callbacks, episode/scope changes and barriers fence coalescing. Backward/zero seeks and source identity are retained. `test_async_playback_history`. |
| Completion/scrobble | Backend and player both completed/scrobbled. | Backend owns one session-scoped transition and serialized local enqueueing. Manual watched uses the same gate; replay/unwatch resets it. Remote generations use the parent series resource to reject obsolete episode jobs. Account episode completion retains its terminal offset so another device can infer completion; local resume is cleared. `test_playback_completion`, continuation resource-key assertions. |
| Saved-source resume | All providers were queried before saved identity was inspected. | Refresh the exact saved provider/release first; require a unique playable identity, never launch a persisted signed URL. Full fallback reuses already fetched provider rows and restores configured provider/release order. Lazy recovery expands remaining providers. `test_playback_resume`. |
| Movie resume | Continue Watching opened the movie detail instead of offering resume/restart/source choice. | Controller dialog offers these actions and the same provider fast path; full detail source list remains available. Requires UI/runtime verification. |
| PS4 cached images | Local cache hit called `setImageFromFile`, bypassing PS4 texture limits. | Local files use existing decode/downscale/upload and request cancellation/grouping; non-PS4/GXM routing retained. Requires GPU/runtime verification. |
| Subtitles | Deduplication kept only one subtitle per language. | Canonical language + ID + URL identity retains forced/SDH/release alternatives and addon order; first eligible preferred track auto-selects. PS4 bitmap/text guards retained. `test_subtitle_identity`. |
| Home/Search/Genres | Movie-first cap, first-catalog result cap, first-catalog genres. | Shared category interleaving, exact-route dedup, at most 12 serial requests for Home and Search with round-robin results, genres union with first capable route per value. Concurrency remains one; no extra worker fanout. `test_catalog_aggregation`, existing route tests. |
| Native anime type | Catalog discovery accepted anime but mapping/playback treated it as series. | Keep native anime resource type through neutral show UI, episode/season codecs, detail, stream and subtitles; series-based anime remains compatible. `test_episode_continuation`. |
| PS4 source ordering | Followup patch overrode safe ranking with playable-only partition. | Stable safe/unknown playable group, then explicit-risk playable group, then placeholders. Preserve addon ordering within each group; retain every PS4 alternative. Shared pure safety helper. `test_ps4_source_order`. |
| Italian audio | Two similar patch-only classifiers could drift; flag/subtitle association was inconsistent. | One pure classifier distinguishes ITA/audio hints from SUB ITA, ITA SUB, Italian subtitles and subtitle flags. `test_stremio_source_audio`. |
| Localization | Resume/autoplay/source/Italian-audio/debrid strings lived in C++/patches. | i18n keys with the existing en-US fallback, including PS4 transformed episode picker. No requirement to manually translate every locale. |

Standalone tests run through `tests/run.sh`. The baseline subtitle test had two
stale size expectations contradicting its own payload; those expectations now
match the payload, without changing production parsing.

Logic tests and CI compilation do not prove PS4 runtime behavior. Hardware checks
must cover controller focus, resume/restart, EOF/seek/pause durability, alternate
source recovery, subtitle selection and oversized offline artwork. No throughput
or latency improvement is claimed without measurement. Never update the console
firmware as part of those checks.
