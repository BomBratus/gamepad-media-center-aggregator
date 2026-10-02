cmake_minimum_required(VERSION 3.18)

# build options
set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

# GMCA is delivered for PS4. Linux is retained only as a local shared-behavior
# test bench and must be selected explicitly by its caller.
option(GMCA_LINUX_TEST_BENCH "Build GMCA on Linux for runtime testing" OFF)
set(GMCA_OPENORBIS_TOOLCHAIN_ACTIVE OFF)
if (DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{OPENORBIS})
    get_filename_component(_gmca_toolchain_path "${CMAKE_TOOLCHAIN_FILE}" REALPATH)
    get_filename_component(_gmca_openorbis_toolchain "$ENV{OPENORBIS}/cmake/ps4.cmake" REALPATH)
    if (_gmca_toolchain_path STREQUAL _gmca_openorbis_toolchain)
        set(GMCA_OPENORBIS_TOOLCHAIN_ACTIVE ON)
    endif ()
endif ()
if (DEFINED CMAKE_SYSTEM_NAME AND NOT CMAKE_SYSTEM_NAME STREQUAL "")
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT GMCA_LINUX_TEST_BENCH)
        message(FATAL_ERROR "Linux is a test bench only; select it explicitly with GMCA_LINUX_TEST_BENCH")
    elseif (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT GMCA_OPENORBIS_TOOLCHAIN_ACTIVE)
        message(FATAL_ERROR "GMCA only supports a PS4 target from the OpenOrbis toolchain or the explicit Linux test bench (unsupported target: ${CMAKE_SYSTEM_NAME})")
    endif ()
endif ()
if (NOT GMCA_LINUX_TEST_BENCH AND DEFINED PLATFORM_DESKTOP AND PLATFORM_DESKTOP)
    message(FATAL_ERROR "Select the Linux test bench explicitly with GMCA_LINUX_TEST_BENCH")
endif ()
if (PLATFORM_SWITCH OR PLATFORM_PSV OR PLATFORM_ANDROID OR PLATFORM_IOS OR PLATFORM_TVOS)
    message(FATAL_ERROR "GMCA supports PS4 delivery and the explicit GMCA_LINUX_TEST_BENCH only")
endif ()
if (DEFINED GMCA_STREMIO_ONLY AND GMCA_STREMIO_ONLY)
    message(FATAL_ERROR "Stremio-only mode is now the only GMCA profile; GMCA_STREMIO_ONLY was removed")
endif ()
if (GMCA_LINUX_TEST_BENCH)
    if (NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
        (DEFINED CMAKE_SYSTEM_NAME AND NOT CMAKE_SYSTEM_NAME STREQUAL "" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        message(FATAL_ERROR "GMCA_LINUX_TEST_BENCH is only supported on Linux")
    endif ()
    set(PLATFORM_PS4 OFF CACHE BOOL "" FORCE)
    set(PLATFORM_DESKTOP ON CACHE BOOL "" FORCE)
else ()
    set(PLATFORM_PS4 ON CACHE BOOL "" FORCE)
    set(PLATFORM_DESKTOP OFF CACHE BOOL "" FORCE)
endif ()

# Include Platform common options
# Linux test bench: cmake -B build -DGMCA_LINUX_TEST_BENCH=ON
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

if (PLATFORM_DESKTOP AND NOT GMCA_LINUX_TEST_BENCH)
    message(FATAL_ERROR "Select the Linux test bench explicitly with GMCA_LINUX_TEST_BENCH")
endif ()
if (GMCA_LINUX_TEST_BENCH AND NOT PLATFORM_DESKTOP)
    message(FATAL_ERROR "GMCA_LINUX_TEST_BENCH requires PLATFORM_DESKTOP")
elseif (NOT GMCA_LINUX_TEST_BENCH AND NOT PLATFORM_PS4)
    message(FATAL_ERROR "GMCA PS4 is the default platform; select Linux with GMCA_LINUX_TEST_BENCH")
endif ()

option(GMCA_TEST_HARNESS "Build the Linux navigation smoke-test harness" OFF)
if (GMCA_TEST_HARNESS)
    if (NOT GMCA_LINUX_TEST_BENCH OR NOT USE_SDL2)
        message(FATAL_ERROR "GMCA_TEST_HARNESS requires GMCA_LINUX_TEST_BENCH and USE_SDL2")
    endif ()
    find_package(SDL2 2.24 REQUIRED)
    list(APPEND APP_PLATFORM_OPTION -DGMCA_TEST_HARNESS)
    list(APPEND APP_PLATFORM_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui)
endif ()

option(USE_SYSTEM_CURL "use system installed libcurl" ON)

# mpv related
# If your system does not support OpenGL(ES), you can use software rendering, but it will affect performance.
option(MPV_SW_RENDER "Using CPU to draw videos" OFF)
if (MPV_SW_RENDER)
    list(APPEND APP_PLATFORM_OPTION -DMPV_SW_RENDER)
endif ()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
elseif (CMAKE_BUILD_TYPE STREQUAL Debug)
    list(APPEND APP_PLATFORM_OPTION -D_DEBUG -D_GLIBCXX_ASSERTIONS)
endif()

# Add git info
if (NOT DEFINED GIT_TAG_SHORT)
    execute_process(COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 5 OUTPUT_VARIABLE GIT_TAG_SHORT
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif ()

if (NOT DEFINED VERSION_BUILD)
    execute_process(COMMAND git rev-list --count --all
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 5 OUTPUT_VARIABLE VERSION_BUILD
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif ()
message(STATUS "git commit ${GIT_TAG_SHORT} build ${VERSION_BUILD}")

# toolchain
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# project info
project(GMCA)  # short token for files/binaries/IDs; display name = "Gamepad Media Center Aggregator"
set(VERSION_MAJOR "1")
set(VERSION_MINOR "1")
set(VERSION_ALTER "0")
set(PROJECT_AUTHOR "thcolin")
set(PACKAGE_NAME "fun.thcolin.gmca")
set(PSN_TITLE_ID "GMCA00000")
# PS4 package revision currently published by the rolling in-app update feed.
set(PS4_VERSION "00.85")
# Stable identity is required for package updates over the installed PS4 title.
set(PS4_CONTENT_ID "IV0001-GMCA00000_00-GMCA000000006700")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

# Embed the changelog (single source of truth at the repo root) into the
# resources so the in-app Changelog screen can read it via romfs /
# BRLS_RESOURCES on every platform. Configure-time copy: picked up by
# libromfs and the resource copy steps; editing CHANGELOG.md re-triggers
# CMake configure. The copy under resources/ is gitignored.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md ${PROJECT_RESOURCES}/CHANGELOG.md COPYONLY)

if (CUSTOM_RESOURCES_DIR)
    set(BRLS_RESOURCES_DIR ${CUSTOM_RESOURCES_DIR})
endif ()

# setting src and include
file(GLOB_RECURSE MAIN_SRC "${PROJECT_SOURCE_DIR}/app/src/*.cpp")
if (GMCA_TEST_HARNESS)
    list(APPEND MAIN_SRC ${PROJECT_SOURCE_DIR}/tests/ui/harness.cpp)
endif ()
if (PLATFORM_PS4)
    # Show backtrace when app crash
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/ps4/crashlog.c)
endif ()

if (USE_LIBROMFS)
    message(STATUS "USE LIBROMFS")
    add_definitions(-DUSE_LIBROMFS)
    set(LIBROMFS_PROJECT_NAME ${PROJECT_NAME})
    set(LIBROMFS_RESOURCE_LOCATION "${PROJECT_RESOURCES}")
endif ()

# build borealis and other third party libraries
add_subdirectory(library)

if (GMCA_TEST_HARNESS)
    target_compile_definitions(borealis PUBLIC GMCA_TEST_HARNESS)
endif ()

if (USE_SYSTEM_CURL)
    find_package(CURL REQUIRED)
else ()
    include(FetchContent)
    FetchContent_Declare(curl
        URL https://curl.se/download/curl-8.16.0.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    find_package(OpenSSL REQUIRED)
    set(CURL_USE_OPENSSL ON)
    set(HTTP_ONLY ON)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(USE_NGHTTP2 OFF)
    set(USE_LIBIDN2 OFF)
    set(CURL_BROTLI OFF)
    set(CURL_ZSTD OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_DISABLE_PROGRESS_METER ON)
    FetchContent_MakeAvailable(curl)
endif ()
list(APPEND APP_PLATFORM_LIB CURL::libcurl)

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

if (USE_BOOST_FILESYSTEM)
    find_package(Boost 1.44 REQUIRED COMPONENTS filesystem)
    if(Boost_FOUND)
        list(APPEND APP_PLATFORM_LIB Boost::filesystem)
        list(APPEND APP_PLATFORM_OPTION -DUSE_BOOST_FILESYSTEM -DBOOST_FILESYSTEM_VERSION=4)
    endif()
endif ()

if (PLATFORM_DESKTOP)
    find_package(MPV REQUIRED)
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIR})
    list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARY})
    find_library(AVFORMAT_LIB NAMES avformat REQUIRED)
    list(APPEND APP_PLATFORM_LIB ${AVFORMAT_LIB})

    find_package(WebP)
    if (WebP_FOUND)
        list(APPEND APP_PLATFORM_OPTION -DUSE_WEBP)
        list(APPEND APP_PLATFORM_INCLUDE ${WebP_INCLUDE_DIRS})
        list(APPEND APP_PLATFORM_LIB ${WebP_LIBRARY})
    endif ()

else ()
    # PS4 cross compile; dependencies are provided by the OpenOrbis toolchain.
    # If dependent files cannot be found, environment variables can be set before executing the cmake command:
    # PKG_CONFIG_LIBDIR=/some/path/arm64/lib/pkgconfig
    find_package(PkgConfig REQUIRED)

    pkg_search_module(MPV REQUIRED mpv)
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIRS})
    list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARIES})
    link_directories(${MPV_LIBRARY_DIRS})

    pkg_search_module(LIBWEBP QUIET libwebp)
    if (LIBWEBP_FOUND)
        message(STATUS "Found libwebp: ${LIBWEBP_INCLUDE_DIRS} ${LIBWEBP_LIBRARIES}")
        list(APPEND APP_PLATFORM_INCLUDE ${LIBWEBP_INCLUDE_DIRS})
        list(APPEND APP_PLATFORM_LIB ${LIBWEBP_LIBRARIES})
        list(APPEND APP_PLATFORM_OPTION -DUSE_WEBP)
        link_directories(${LIBWEBP_LIBRARY_DIRS})
    endif ()
endif()

# building target
program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PRIVATE app/include ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections -Wunused-variable ${APP_PLATFORM_OPTION})
target_link_libraries(${PROJECT_NAME} PRIVATE borealis lunasvg ${APP_PLATFORM_LIB})
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION=${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}
    APP_BUILD=${VERSION_BUILD}
    BUILD_TAG_SHORT=${GIT_TAG_SHORT}
    BUILD_PACKAGE_NAME=${PROJECT_NAME}
)
if (PLATFORM_PS4)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        GMCA_PS4_SAFE_SOURCES=1
        GMCA_PS4_VERSION=${PS4_VERSION})
    set_property(SOURCE app/src/utils/ps4_update.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        GMCA_PS4_VERSION=${PS4_VERSION})
elseif (GMCA_LINUX_TEST_BENCH)
    target_compile_definitions(${PROJECT_NAME} PRIVATE GMCA_LINUX_TEST_BENCH=1)
endif ()

# building release file
if (PLATFORM_DESKTOP)
    if (NOT USE_LIBROMFS)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${PROJECT_BINARY_DIR}/resources
        )
    endif()

elseif (PLATFORM_PS4)
    target_link_libraries(${PROJECT_NAME} PRIVATE SceSystemService)
    add_self(${PROJECT_NAME})
    include(${CMAKE_SOURCE_DIR}/cmake/gmca_ps4_pkg.cmake)
    gmca_add_pkg(${PROJECT_NAME}
        ${CMAKE_BINARY_DIR}/ps4
        ${PSN_TITLE_ID}
        ${PROJECT_NAME}
        ${PS4_VERSION}
        ${PS4_CONTENT_ID}
        8388608)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${BOREALIS_DIR}/ps4/sce_module ${CMAKE_BINARY_DIR}/ps4/sce_module
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/app/platform/ps4/sce_sys ${CMAKE_BINARY_DIR}/ps4/sce_sys
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${BOREALIS_DIR}/ps4/sce_sys/about ${CMAKE_BINARY_DIR}/ps4/sce_sys/about
        COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/ps4/resources
    )
endif ()
