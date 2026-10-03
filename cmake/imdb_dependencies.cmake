# Pinned, portable C sources: PS4 uses neither a host SQLite library nor mmap.
include(FetchContent)
FetchContent_Declare(gmca_sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(gmca_sqlite)
if (NOT gmca_sqlite_POPULATED)
    FetchContent_Populate(gmca_sqlite)
endif ()
add_library(gmca_sqlite STATIC ${gmca_sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(gmca_sqlite PUBLIC ${gmca_sqlite_SOURCE_DIR})
target_compile_definitions(gmca_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_WAL
    SQLITE_MAX_MMAP_SIZE=0 SQLITE_DEFAULT_CACHE_SIZE=-4096
    SQLITE_TEMP_STORE=1 SQLITE_DEFAULT_MEMSTATUS=0)
if (PLATFORM_PS4)
    target_compile_definitions(gmca_sqlite PRIVATE SQLITE_OS_UNIX=1)
endif ()
target_link_libraries(gmca_sqlite PUBLIC ${CMAKE_THREAD_LIBS_INIT})

FetchContent_Declare(gmca_zlib
    URL https://zlib.net/fossils/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(gmca_zlib)
if (NOT gmca_zlib_POPULATED)
    FetchContent_Populate(gmca_zlib)
endif ()
set(_gmca_zlib_sources adler32.c crc32.c deflate.c infback.c inffast.c inflate.c
    inftrees.c trees.c zutil.c compress.c uncompr.c gzclose.c gzlib.c gzread.c gzwrite.c)
list(TRANSFORM _gmca_zlib_sources PREPEND "${gmca_zlib_SOURCE_DIR}/")
add_library(gmca_index_zlib STATIC ${_gmca_zlib_sources})
target_include_directories(gmca_index_zlib PUBLIC ${gmca_zlib_SOURCE_DIR})
target_compile_definitions(gmca_index_zlib PRIVATE Z_HAVE_UNISTD_H)
