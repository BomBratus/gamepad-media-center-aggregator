# GMCA PS4 package helper.
#
# PacBrew's add_pkg() derives CONTENT_ID from the package version. That is fine
# for one-off homebrew builds, but GMCA installs successive full packages over
# the same TITLE_ID. Keep the content identity stable while allowing APP_VER /
# VERSION and the downloadable filename to advance for every test build.

function(gmca_add_pkg project pkgdir title_id title version content_id)
    string(SUBSTRING "${title}" 0 127 title_clean)

    string(SUBSTRING "${version}" 0 7 version_clean)
    string(REGEX MATCH "([0-9]+\\.[0-9]+)" version_clean "${version_clean}")
    if("${version_clean}" STREQUAL "")
        message(FATAL_ERROR "Invalid GMCA PS4 version '${version}'")
    endif()

    string(LENGTH "${content_id}" content_id_length)
    if(NOT content_id_length EQUAL 36 OR NOT "${content_id}" MATCHES "^[A-Z0-9_-]+$")
        message(FATAL_ERROR "Invalid GMCA PS4 content id '${content_id}'")
    endif()

    # Keep the on-disk PKG name versioned even though the package CONTENT_ID is
    # intentionally stable. The filename is only a transport artifact; the PS4
    # reads identity/version from the package/SFO metadata.
    string(REPLACE "." "0" version_suffix "${version_clean}")
    string(APPEND version_suffix "00000000")
    string(SUBSTRING "${version_suffix}" 0 7 version_suffix)
    set(versioned_content_id "IV0001-${title_id}_00-${title_id}${version_suffix}")
    set(versioned_pkg "${versioned_content_id}.pkg")
    set(PKG_OUT_NAME "${versioned_pkg}" CACHE STRING "ps4 pkg name" FORCE)

    set(attribute 0)
    if(${ARGC} GREATER 6)
        set(attribute ${ARGV6})
    endif()

    set(category "gde")
    if(${ARGC} GREATER 7)
        set(category "${ARGV7}")
    endif()

    set(PKGTOOL "${OPENORBIS}/bin/linux/PkgTool.Core")
    set(DOTFIX "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1")

    add_custom_command(
        OUTPUT "${versioned_pkg}"
        COMMAND ${CMAKE_COMMAND} -E copy eboot.bin ${pkgdir}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_new ${pkgdir}/sce_sys/param.sfo
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo APP_TYPE --type Integer --maxsize 4 --value 1
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo APP_VER --type Utf8 --maxsize 8 --value "${version_clean}"
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo ATTRIBUTE --type Integer --maxsize 4 --value ${attribute}
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo CATEGORY --type Utf8 --maxsize 4 --value "${category}"
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo FORMAT --type Utf8 --maxsize 4 --value "obs"
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo CONTENT_ID --type Utf8 --maxsize 48 --value "${content_id}"
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo SYSTEM_VER --type Integer --maxsize 4 --value 1020
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo TITLE --type Utf8 --maxsize 128 --value "${title_clean}"
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo TITLE_ID --type Utf8 --maxsize 12 --value "${title_id}"
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} sfo_setentry ${pkgdir}/sce_sys/param.sfo VERSION --type Utf8 --maxsize 8 --value "${version_clean}"
        COMMAND "${OPENORBIS}/bin/linux/create-gp4" -out ${pkgdir}/${project}.gp4 --content-id "${content_id}" --path "${pkgdir}"
        COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${PKGTOOL} pkg_build ${pkgdir}/${project}.gp4 ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/${versioned_pkg}"
        COMMAND ${CMAKE_COMMAND} -E rename "${CMAKE_BINARY_DIR}/${content_id}.pkg" "${CMAKE_BINARY_DIR}/${versioned_pkg}"
        COMMAND ${CMAKE_COMMAND} -E remove ${pkgdir}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E remove ${pkgdir}/sce_sys/param.sfo
        COMMAND ${CMAKE_COMMAND} -E remove ${pkgdir}/${project}.gp4
        VERBATIM
        DEPENDS "${project}.self"
    )

    add_custom_target(
        "${project}_pkg" ALL
        DEPENDS "${versioned_pkg}"
    )
endfunction()
