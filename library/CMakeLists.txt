cmake_minimum_required(VERSION 3.15)

# Apply the small UI patch here as well as the platform-wide Borealis patch
# applied by CI. Keeping this separate avoids regenerating the large shared
# patch for a one-line Dropdown behaviour fix and makes local builds consistent.
set(BOREALIS_DROPDOWN_OVERFLOW_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/../scripts/patches/borealis-dropdown-overflow.patch")
if (EXISTS "${BOREALIS_DROPDOWN_OVERFLOW_PATCH}")
    execute_process(
        COMMAND git apply --check "${BOREALIS_DROPDOWN_OVERFLOW_PATCH}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/borealis"
        RESULT_VARIABLE BOREALIS_DROPDOWN_PATCH_CAN_APPLY
        OUTPUT_QUIET ERROR_QUIET)
    if (BOREALIS_DROPDOWN_PATCH_CAN_APPLY EQUAL 0)
        execute_process(
            COMMAND git apply "${BOREALIS_DROPDOWN_OVERFLOW_PATCH}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/borealis"
            RESULT_VARIABLE BOREALIS_DROPDOWN_PATCH_RESULT)
        if (NOT BOREALIS_DROPDOWN_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to apply Borealis dropdown overflow patch")
        endif ()
    else ()
        # Re-configuring an existing build is normal: accept the patch when it
        # is already present, but warn if the source drifted in another way.
        execute_process(
            COMMAND git apply --reverse --check "${BOREALIS_DROPDOWN_OVERFLOW_PATCH}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/borealis"
            RESULT_VARIABLE BOREALIS_DROPDOWN_PATCH_ALREADY_APPLIED
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT BOREALIS_DROPDOWN_PATCH_ALREADY_APPLIED EQUAL 0)
            message(WARNING "Borealis dropdown overflow patch does not apply cleanly")
        endif ()
    endif ()
endif ()

# add lunasvg
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
add_subdirectory(lunasvg EXCLUDE_FROM_ALL)

# add borealis
add_subdirectory(borealis/library)

if (PLATFORM_PS4)
    list(APPEND APP_PLATFORM_LIB SceNet mbedcrypto mbedtls mbedx509)
    set(APP_PLATFORM_LIB ${APP_PLATFORM_LIB} PARENT_SCOPE)
elseif (PLATFORM_PSV)
    list(APPEND APP_PLATFORM_LIB SceVshBridge_stub SceAppMgr_stub)
    set(APP_PLATFORM_LIB ${APP_PLATFORM_LIB} PARENT_SCOPE)
endif ()