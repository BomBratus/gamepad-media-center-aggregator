#include "api/stremio/source_audio.hpp"
#include <cassert>
int main() {
    using stremio::hasItalianAudio;
    for (auto text : {"ITA", "Italian Audio", "Dual ITA", "Audio Italiano", u8"🇮🇹", u8"Audio 🇮🇹", "ITA audio / SUB ENG"})
        assert(hasItalianAudio(text));
    for (auto text : {"SUB ITA", "ITA SUB", "Italian subtitles", "Italian subs", "SUB-ITA", "Italian movie", u8"SUB 🇮🇹", u8"🇮🇹 subtitles", u8"🇮🇹 Movie Italian subtitles", "Sottotitoli ITA", "Digital", "Vital"})
        assert(!hasItalianAudio(text));
}
