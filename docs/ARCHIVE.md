# IMDb Archive Query Engine v2

Archive downloads `title.ratings.tsv.gz`, `title.basics.tsv.gz` and
`title.akas.tsv.gz` directly from `https://datasets.imdbws.com/`. IMDb's
personal/non-commercial dataset terms apply. Datasets are not shipped in GMCA
packages or the update channel. Stremio addons still supply detail, synopsis,
episodes and streams; visible posters use IMDb-ID Metahub URLs.

## Full rebuild and immutable generations

A weekly refresh (or Settings → Refresh Archive) downloads and imports a complete
replacement into `imdb-index-v1.sqlite.building`. The historical filename is kept
so format v1 is detected; `settings.version=2` identifies the incompatible new
format. v1 readers are rejected cleanly and the cache automatically schedules a
full rebuild. No compatibility query engine runs alongside v2.

Import checkpoints every 2,000 input rows and completed downloads survive a
playback interruption/restart. Derived indexes are rebuilt from the completed
import if interrupted; a `derived_ready` checkpoint allows retrying compaction
without losing the import. This is recovery of a full rebuild, not incremental
catalog maintenance. An invalid/truncated dataset discards its staging database
and downloads. Other failures retain resumable work and retry with backoff.

The writer builds records, facets, search postings and sort arrays, validates
SQLite integrity, counts, numeric bounds, sort permutations and search references,
then closes all statements/connections before an atomic rename. A failed build
never replaces the published file. New titles become visible together. Published
databases are read-only; old connections retain the old inode across replacement.
`Snapshot` ownership keeps a browse session on its generation. New resets can
capture the latest generation. Cursors additionally carry a process-unique reader
identity, immutable match state, predicates, sort/direction and sort position;
using an A cursor on reader B, or changing predicates/order, is rejected.

## Integer keys and facets

Each browsable title receives a dense, zero-based `title_key` in **IMDb ID order**
during the derived build, independently of input dataset order. Keys are local to
one generation; the `records` table preserves the unique `title_key ↔ IMDb ID`
relationship. External detail/playback identifiers remain IMDb IDs. There is no
metadata JSON snapshot or alias/string collection loaded into RAM.

Eligibility remains ≥100 rating votes, excluding episodes/video games, with adult
titles hidden. On 2026-10-04 the live ratings/basics datasets contained **237,138
browsable titles** under these rules (432,441 rated ≥100; 194,214 episodes/games;
1,089 adult titles). This count is a dated measurement, not a permanent limit.

Type and each genre have a packed bitset. Year (16 bits), raw rating in tenths
(7 bits) and votes (32 bits) use **bit-sliced indexes**: one bitset per numeric
bit. A most-significant-bit comparator produces exact ≥ threshold sets, and year
ranges AND the two bounds. Arbitrary vote thresholds remain exact; there are no
coarse buckets requiring metadata scans. IMDb ratings must be finite tenths in
[0,10], years [0,65535] and votes [0,UINT32_MAX]; unexpected data fails validation
instead of truncating. Missing year is zero and excluded by an upper year bound.

The matcher composes bitset AND operations and caches the resulting membership
bitset plus sorted matching integer keys. Work is O(selected planes × N/64),
plus enumeration of matching keys, not N SQLite title-row reads/materialization.
Changing only sort, requesting another cursor page or Random reuses that exact
match state. Unknown/unsupported facets yield no matches. Genre options and
browsable count are small build-time metadata tables.

This representation avoids thousands of distinct vote postings, repeated seeks,
combinatorial indexes, heavy dependencies and heap objects per title. Arrays are
contiguous, facets are loaded on demand and retained for one reader generation.

## Sort, cursors and Random

Four ascending integer arrays are built offline: year/release, Bayesian IMDb
score, votes and case-folded display name. Only the selected array is loaded.
Tie-group boundaries permit descending traversal while preserving IMDb-ID
ascending ties, without a second descending array. Added/Updated use key order,
as in the previous SQLite engine. Displayed ratings and minimum-rating predicates
use **raw** IMDb rating; Rating sort uses the existing Bayesian formula with
M=25,000 and C=average rating for titles with ≥1,000 votes.

Each page continues from its cursor position in the selected sort array, checks
membership, and fetches only visible `records` by integer primary key. There is
no SQL `OFFSET`, result-set sort, repeated count, temp `archive_results` or live
mutation. A selective first page may traverse much/all of the integer order
array, especially when its matches lie at the far end; this reads no metadata
and subsequent pages continue rather than restarting. A sort change may perform
one sequential blob read to load the new array. Deep pagination visits the order
array once across the entire session, without duplicate cards.

Random samples one index uniformly from the complete cached matching-key vector.
It never samples only the displayed page and never uses `ORDER BY RANDOM()`.

## Substring search

The custom index stores sorted integer postings for distinct 1-, 2- and 3-byte
sequences across each title's supported names: primary, original, Italian AKA,
US/GB/CA/AU/XWW AKA and English-language AKA. ASCII case-folding matches the
previous SQLite `lower()` semantics; Unicode bytes remain unchanged. Literal
`%`/`_` retain substring meaning. Names deduplicate by title and by string.

Postings reside in SQLite blobs, in 16,384-key little-endian chunks. Small term
metadata ranks query trigrams by posting count; their sorted postings are
intersected and restricted by facets before verification. For queries longer
than three bytes, only candidate title keys retrieve their indexed names to
verify contiguous substring occurrence. This removes false positives from grams
in different aliases/positions. No `instr()` scan of titles or aliases remains.
A common long substring can still require many candidate name reads; those costs
are explicitly measured and are a PS4 hardware-validation risk.

The explicit short-search path uses the precomputed unigram/bigram postings;
it preserves complete substring results, including one/two-character queries,
without scanning all names. UTF-8 queries are indexed consistently as bytes,
including multibyte characters. No FTS5 is enabled: the pinned OpenOrbis SQLite
build does not define `SQLITE_ENABLE_FTS5`, and the custom format has predictable
ownership/build requirements without adding tokenizer dependencies or changing
substring semantics.

## Memory and disk policy

A reader enforces ≤2,000,000 titles and ≤64 genre options. For N titles, each
facet requires `8 × ceil(N/64)` bytes. There are 57 non-genre planes
(2 type + 16 year + 7 rating + 32 votes), so the hard cap is 121 planes:
**15.125 × N bytes** maximum facet payload (~28.85 MiB at 2M), loaded lazily.
A match uses N/8 bytes plus 4 bytes per match. One selected sort array costs 4N,
and its tie boundaries at most another 4N. Numeric comparisons temporarily use
three N/8 bitsets; search intersections use at most two 4N candidate arrays plus
one bounded posting chunk. SQLite's reader cache is 4 MiB, with mmap disabled.

Worst-case retained payload per reader is approximately **27.25N bytes + 4 MiB**
(facets + current sort/ties + one all-title match). Allow roughly **44N bytes +
4 MiB** transiently for decoding SQLite blobs, comparisons/search, vector storage
and one additional active cursor state: ~88 MiB at the 2M hard cap, ~14 MiB at the
measured 237k count. These are conservative payload estimates, not measured PS4
heap peaks; allocator/SQLite statement overhead and UI cards are additional.
An old pinned generation and a newly published reader can coexist, so budget two
readers (~176 MiB at the artificial cap), plus the staging builder (~15.125N
facet payload, SQLite cache, current-title terms and disk-backed sorts). Each
extra externally retained cursor adds up to 4.125N bytes; the UI retains one.
Sort arrays and all aliases are not loaded together. There is no mmap reliance.

Four sorts plus tie boundaries occupy at most 32N bytes on disk; facets at most
15.125N. Search payload is **4P bytes**, where P is total distinct per-title
1/2/3-byte sequences, plus term/chunk B-tree overhead. Records, unique IMDb IDs
and searchable name strings are additional and depend on actual name lengths.
Staging temporarily holds both import tables and an uncompressed `(term,key)`
B-tree before compacting postings, and VACUUM needs temporary space. Plan disk
for the old generation, staging, compaction and compressed datasets together.
Published/build size and build duration must be measured on the actual dataset;
the synthetic benchmark reports its database bytes and process peak RSS.

## PS4, playback and diagnostics

The dedicated serial Archive query worker remains separate from addon/network
work, with latest-wins cancellation, SQLite progress cancellation and explicit
cancellation checks between bit planes, posting chunks and order-scan batches.
Controls keep the 150ms debounce; loading feedback and callback invalidation are
immediate. Controller focus, filters, skeleton, Reset, refresh/progress, pagination
and Random remain in the existing UI. Random uses the same pinned snapshot.

`PlaybackGate` cancels download/import/index derivation before opening a stream;
cleanup/checkpoint completion runs off the playback UI path. Work resumes after
playback. Exit cancels and waits for staging cleanup and stops the reader queue
before application/static teardown. Published readers never contend with a
writer to their database. `gmca-ps4-index` wraps SQLite's `unix-none` VFS to copy
absolute app-private paths; OpenOrbis lacks the parent `lstat`/`readlink` behavior.
Do not introduce relative/user paths, concurrent staging readers or a live writer.

SQLite 3.53.4 and zlib 1.3.1 sources are pinned/hash-verified by
`cmake/imdb_dependencies.cmake`, compiled by the real OpenOrbis workflow with
mmap/WAL disabled. Current `dev` applies `scripts/patches/borealis-fixes.patch`.
Historical `custom/ps4-safe-sources.patch` / `custom/ps4-ui-followups.patch` are
absent from current `dev` and are not part of that workflow; the maintained
PS4 source paths use `GMCA_PS4_SAFE_SOURCES` directly.

PS4 query logs contain generation, filter/search-index/sort-scan/metadata/total
milliseconds, matches/items, cursor in/out, reuse and work counters; queue timing
is separate. No user search text, credentials or media URLs are logged. Build
logs record phase, titles, index and elapsed milliseconds. Metrics also expose
SQLite full-scan steps and sort count for tests.

## Verification

Run `./tests/run.sh` or configure `tests/imdb` and run CTest. Set
`GMCA_JSON_INCLUDE` to an existing Borealis JSON tree on a Linux test bench.
Checks cover importer recovery, aliases/substrings/short search, predicates,
Bayesian score, ties, cursor traversal/reuse/rejection, Random over the full set,
failed rebuild/old-reader survival, cancellation, query queue and PlaybackGate.
The PS4 VFS test injects `lstat=ENOSYS` before import/query.

The opt-in `GMCA_ARCHIVE_BENCHMARK=ON` target `benchmark_archive [titles]` defaults
to 500,000 records. It reports no-filter cold/warm, type, genre, year, raw rating,
votes, combined facets, common/absent/short search, sort change, first/deep page
and Random. It asserts zero SQL full-scan/sort steps, metadata reads only for
visible cards, match reuse and no repeated order traversal in deep pagination.
There are no tight wall-clock CI thresholds. `Archive engine checks` compares
identical synthetic fixtures against baseline `ef7d347e` on one Linux runner;
that temporary baseline binary is a benchmark control, never an app fallback.

Linux/CI checks establish shared correctness and measured Linux costs. OpenOrbis
compile, PKG validation and artifact upload establish build compatibility only.
Report **build-validated, hardware performance pending** until tested on a real
PS4. Never infer console latency, startup heap or playback stutter from Linux
benchmarks or successful packaging. Console firmware is never part of delivery.
