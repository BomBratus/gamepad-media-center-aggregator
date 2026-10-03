# Local IMDb archive

Archive uses the same bulk IMDb source and local SQLite approach as the TV box.
GMCA downloads `title.ratings.tsv.gz`, `title.basics.tsv.gz`, and
`title.akas.tsv.gz` directly from `https://datasets.imdbws.com/`, then imports
rows on disk. It does not crawl Stremio catalog pages or depend on a TV-box
service. IMDb's personal/non-commercial dataset terms apply; datasets are not
bundled in the package or published to the update channel.

The index includes titles with at least 100 IMDb votes, excluding individual
TV episodes and video games, matching the TV-box builder. Adult titles are
excluded from browsing. Movie/TV-movie types, series/miniseries, shorts and other
non-episode types use the usual Stremio movie/series detail flow. Italian display
titles use the first Italian AKA where available; original and English AKA
titles remain searchable. Title details, episode lists, synopsis and streams
still come from the user's Stremio addons when opened. Posters use the same
IMDb-ID Metahub URLs as the TV box, downloaded only for visible cards.

Search, movie/series type, genre, year/range, rating and IMDb vote filters operate
on the whole database before paging. Rating order uses the TV box's Bayesian
score (M=25,000, C from titles with at least 1,000 votes); displayed ratings and
minimum-rating filters use the raw IMDb rating. Random samples the entire
matching set. Votes are IMDb rating votes, not public views or the user's
watched count. Country, streaming-service, addon-provenance and synopsis filters
are hidden because these bulk datasets do not supply those fields.

The first build still needs time and disk space for compressed datasets and the
SQLite database. Downloads stream to disk, gzip rows are read in bounded buffers,
and SQLite uses a 4 MiB page cache and disk-backed temporary sorting. There is
no full metadata snapshot copied into RAM. Completed downloads and import
checkpoints (every 2,000 input rows) are reused after cancellation/restart.
An interrupted incomplete HTTP download restarts that one file; already completed
files are retained. Progress displays the number of imported titles.

Playback cancels the background request/import. The player waits on a worker
thread for checkpoint/cleanup before loading the stream; the UI remains
responsive, and watching does not require waiting for the whole index. Indexing
resumes when the player closes, including pending scheduled/manual refreshes.
The application cannot index after it is closed. Completed indexes refresh
weekly, with a ten-minute retry backoff on failure. Settings → Refresh archive
forces a rebuild and opens Archive to show progress.

`imdb-index-v1.sqlite` is shared across profiles under the GMCA configuration
directory. Refreshes build in a separate `.building` database and atomically
replace the published file only on success. Queries page through a read-only
connection to one published generation, so an ongoing refresh cannot reorder
existing pages. Old scoped `archive-<hash>.json` addon caches are preserved on
disk but are no longer loaded or crawled.

Local Archive queries use their own serial reader worker, independently of the
addon/network task queue. Reopening an index seeks its small genre list instead
of scanning every title/genre pair; new indexes also store the browsable count.
Existing published indexes remain readable without a new download. SQLite
reader work has a 15-second execution budget so expensive queries report an
error and clear the loading state. Index opening and result-query stages are
recorded in the PS4 diagnostic log without filter text or credentials.

PS4 uses `gmca-ps4-index`, a wrapper around SQLite's `unix-none` VFS. It copies
the already-absolute private index paths instead of resolving their parents
through `lstat`/`readlink`, which OpenOrbis musl does not implement. Every path
passed to this VFS must be generated under the writable app directory; do not
use it for relative paths or arbitrary user-supplied database names. The file
lifecycles need no POSIX byte-range locks: one staging writer, no concurrent
staging readers, and only read-only connections to published files. Do not add
a second writer or mutate a published database in place. SQLite's rollback
journal remains enabled for staging recovery, and SQLite temporary files use
the writable config directory.
SQLite/zlib C sources are pinned and hash-verified in
`cmake/imdb_dependencies.cmake`; they are compiled by remote PS4 CI.

Run `./tests/run.sh` for the importer/query, playback gate and existing standalone
checks. `GMCA_JSON_INCLUDE` can point at an existing Borealis JSON include tree
on a Linux test bench. Fixture checks cover import interruption/resume, Italian
and English aliases, filters, paging, random picks, and failed refresh preserving
the old database. The PS4 path regression injects `lstat=ENOSYS`, verifies the
original Unix VFS cannot open the database, and then runs the complete importer
through the PS4 wrapper under that same condition. Archive lifecycle, dataset
download stages, and failure reasons are recorded in the PS4 diagnostic log.
Linux tests establish shared behavior; console timings and
system-crash resolution still need the user's PS4 test after package delivery.
