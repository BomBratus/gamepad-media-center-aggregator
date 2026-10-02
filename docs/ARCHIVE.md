# Local Stremio archive

Archive is available in the Stremio sidebar. Settings → Refresh archive starts a
manual refresh and opens Archive to show progress. The archive also refreshes
at startup when three days old, with an hourly age check while GMCA stays open.
The console cannot run this job after GMCA closes. No TV-box service is needed.

The downloader enumerates the configured addons' browsable movie/series
catalogs. It advances `skip` by the actual number of returned previews,
recognizes `hasMore: false`, and stops repeated pages from addons ignoring
pagination. A title is identified by `(Stremio type, id)` and deduplicated across
catalogs; its catalog-addon provenance is retained. Unavailable catalogs do not
prevent other catalogs from being indexed. This is the set of titles the addons
expose for enumeration, not a promise to contain every title in existence.

Search, type, genre, country, year/range, minimum rating, views, streaming service,
catalog addon and other filters apply to the entire cached set before paging.
Random picks uniformly from that same matching set, independently of sorting or
the visible page, then opens the normal title details/source flow. An empty set
shows a message and never falls back to an unfiltered random pick. Year accepts
`2020` or `1990-2020`; clearing it removes the year filter. Triangle (Y) is a
Random shortcut while the Archive controls/grid have focus.

Country, streaming service and public views filters use catalog-provided fields
only. Missing values are not inferred from addon names, IMDb votes, the user's
watched count, or localized titles. The UI marks fields unavailable when no
catalog supplies them. Catalog addon is a separate provenance filter. Addition
and update sorts refer to dates in the local archive; release sorting uses the
release date when provided, falling back to the release year in previews.

Caches live under the GMCA config directory as `archive-<scope hash>.json`.
The scope includes the account/server and configured addon transports. Raw
credentials do not appear in filenames or new diagnostic messages. Cached
metadata is intentionally compact and does not contain resolved stream URLs.
Writes replace the previous file atomically. Progress is checkpointed every
1,000 new titles or 60 seconds; a cancelled first download can retain useful
partial data. Existing entries remain available during refreshes and provider
outages. A changed profile/addon configuration cancels the old job; shutdown
also cancels its HTTP requests before the HTTP pool is joined.

The job uses one HTTP-pool slot instead of Borealis's serial async queue. Its
requests are serial, with a short pause between pages. Limits of 50,000 records,
64 MiB of serialized metadata, 2,000 page requests and 30 minutes per pass keep
console memory, disk and network work bounded. Incomplete enumeration is labelled
**Partial catalog coverage**. Failed network passes retry with a ten-minute
backoff while Archive is open; successful limited passes use the normal three-day
schedule. Metadata already archived is retained when a provider later omits it.

Focused standalone checks:

```sh
c++ -std=gnu++17 -O2 -Wall -Iapp/include \
  -Ilibrary/borealis/library/include/borealis/extern \
  tests/test_stremio_archive.cpp -o /tmp/gmca-test-archive
/tmp/gmca-test-archive
```

PS4 packaging uses the normal Stremio-only build. Desktop controller smoke tests
are a shared-behavior test bench; they do not establish PS4 delivery or console
performance. Do not publish an update or transfer/install a package without the
user's authorization.
