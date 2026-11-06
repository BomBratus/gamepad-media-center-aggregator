#include "api/stremio/source_label.hpp"
#include <cassert>
#include <string>

int main() {
    using stremio::sourceDisplayName;
    using stremio::sourceFirstLine;
    using stremio::sourcePickerLabel;

    // Addons put the release name on the first line and seeders/size below it.
    assert(sourceFirstLine("Release.Name\nseeders 12") == "Release.Name");
    assert(sourceFirstLine("Release.Name\r\nseeders 12") == "Release.Name");
    assert(sourceFirstLine("Release.Name  ") == "Release.Name");
    assert(sourceFirstLine("Release.Name") == "Release.Name");
    assert(sourceFirstLine("\nseeders 12").empty());
    assert(sourceFirstLine("").empty());

    // The release title is the name; the stream name and addon are fallbacks.
    media::Media m;
    m.label = "Torrentio";
    m.sourceName = "Torrentio\n4k";
    m.sourceTitle = "Movie.2024.1080p.WEB-DL.DDP5.1.H.264-GROUP\n👤 1234 💾 4.2 GB ⚡";
    m.detail = "H264  ·  4.2 GB";
    assert(sourceDisplayName(m) == "Movie.2024.1080p.WEB-DL.DDP5.1.H.264-GROUP");
    assert(sourcePickerLabel(m) ==
        "Movie.2024.1080p.WEB-DL.DDP5.1.H.264-GROUP · Torrentio · H264  ·  4.2 GB");

    // No release title: the stream name is used and the addon is not repeated.
    media::Media streamNameOnly;
    streamNameOnly.label = "Torrentio";
    streamNameOnly.sourceName = "Torrentio\n4k";
    streamNameOnly.detail = "H264  ·  4.2 GB";
    assert(sourceDisplayName(streamNameOnly) == "Torrentio");
    assert(sourcePickerLabel(streamNameOnly) == "Torrentio · H264  ·  4.2 GB");

    // Only the addon name survives: the row is still identifiable.
    media::Media addonOnly;
    addonOnly.label = "Comet";
    assert(sourceDisplayName(addonOnly) == "Comet");
    assert(sourcePickerLabel(addonOnly) == "Comet");

    // A source with no name at all renders empty rather than a stray separator.
    media::Media unnamed;
    assert(sourceDisplayName(unnamed).empty());
    assert(sourcePickerLabel(unnamed).empty());

    // Metadata alone never starts the row with a separator.
    media::Media detailOnly;
    detailOnly.detail = "H264  ·  4.2 GB";
    assert(sourcePickerLabel(detailOnly) == "H264  ·  4.2 GB");
}
