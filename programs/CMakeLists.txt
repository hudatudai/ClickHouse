add_compile_options("$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:${COVERAGE_FLAGS}>")

if (USE_CLANG_TIDY)
    set (CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_PATH}")
endif ()

set(MAX_LINKER_MEMORY 3500)
include(../cmake/limit_jobs.cmake)

include(${ClickHouse_SOURCE_DIR}/cmake/split_debug_symbols.cmake)

# The `clickhouse` binary is a multi purpose tool that contains multiple execution modes (client, server, etc.),
# So client/server/... is just a symlink to `clickhouse` binary.
#
# But, there are several components that requires extra libraries, like keeper
# requires NuRaft, that regular binary does not requires, so you can disable
# compilation of this components.
#
# If you do not know what modes you need, turn then all.
option (ENABLE_CLICKHOUSE_ALL "Enable all ClickHouse modes by default" ON)

# https://presentations.clickhouse.com/matemarketing_2020/
option (ENABLE_CLICKHOUSE_GIT_IMPORT "A tool to analyze Git repositories" ${ENABLE_CLICKHOUSE_ALL})

option (ENABLE_CLICKHOUSE_KEEPER "ClickHouse alternative to ZooKeeper" ${ENABLE_CLICKHOUSE_ALL})

option (ENABLE_CLICKHOUSE_KEEPER_CONVERTER "Util allows to convert ZooKeeper logs and snapshots into clickhouse-keeper snapshot" ${ENABLE_CLICKHOUSE_ALL})

option (ENABLE_CLICKHOUSE_KEEPER_CLIENT "ClickHouse Keeper Client" ${ENABLE_CLICKHOUSE_ALL})

if (NOT ENABLE_NURAFT)
    # RECONFIGURE_MESSAGE_LEVEL should not be used here,
    # since ENABLE_NURAFT is set to OFF for FreeBSD and Darwin.
    message (STATUS "clickhouse-keeper and clickhouse-keeper-converter will not be built (lack of NuRaft)")
    set(ENABLE_CLICKHOUSE_KEEPER OFF)
    set(ENABLE_CLICKHOUSE_KEEPER_CONVERTER OFF)
endif()

message(STATUS "ClickHouse extra components:")

if (ENABLE_CLICKHOUSE_SELF_EXTRACTING)
    message(STATUS "Self-extracting executable: ON")
else()
    message(STATUS "Self-extracting executable: OFF")
endif()

if (ENABLE_CLICKHOUSE_KEEPER_CONVERTER)
    message(STATUS "ClickHouse keeper-converter mode: ON")
else()
    message(STATUS "ClickHouse keeper-converter mode: OFF")
endif()

if (ENABLE_CLICKHOUSE_KEEPER)
    message(STATUS "ClickHouse Keeper: ON")
else()
    message(STATUS "ClickHouse Keeper: OFF")
endif()

if (ENABLE_CLICKHOUSE_KEEPER_CLIENT)
    message(STATUS "ClickHouse keeper-client mode: ON")
else()
    message(STATUS "ClickHouse keeper-client mode: OFF")
endif()

configure_file (config_tools.h.in ${CONFIG_INCLUDE_PATH}/config_tools.h)

macro(clickhouse_target_link_split_lib target name)
    target_link_libraries(${target} PRIVATE clickhouse-${name}-lib)
endmacro()

macro(clickhouse_program_add_library name)
    string(TOUPPER ${name} name_uc)
    string(REPLACE "-" "_" name_uc ${name_uc})

    # Some dark magic
    set(CLICKHOUSE_${name_uc}_SOURCES ${CLICKHOUSE_${name_uc}_SOURCES} PARENT_SCOPE)
    set(CLICKHOUSE_${name_uc}_LINK ${CLICKHOUSE_${name_uc}_LINK} PARENT_SCOPE)
    set(CLICKHOUSE_${name_uc}_INCLUDE ${CLICKHOUSE_${name_uc}_INCLUDE} PARENT_SCOPE)

    add_library(clickhouse-${name}-lib ${CLICKHOUSE_${name_uc}_SOURCES})

    set(_link ${CLICKHOUSE_${name_uc}_LINK}) # can't use ${} in if()
    if(_link)
        target_link_libraries(clickhouse-${name}-lib ${CLICKHOUSE_${name_uc}_LINK})
    endif()

    target_include_directories (clickhouse-${name}-lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set(_include ${CLICKHOUSE_${name_uc}_INCLUDE}) # can't use ${} in if()
    if (_include)
        target_include_directories(clickhouse-${name}-lib ${CLICKHOUSE_${name_uc}_INCLUDE})
    endif()
endmacro()

macro(clickhouse_program_add name)
    clickhouse_program_add_library(${name})
endmacro()

add_subdirectory (bash-completion)
add_subdirectory (benchmark)
add_subdirectory (check-marks)
add_subdirectory (checksum-for-compressed-block)
add_subdirectory (client)
add_subdirectory (compressor)
add_subdirectory (disks)
add_subdirectory (extract-from-config)
add_subdirectory (format)
add_subdirectory (fst-dump-tree)
add_subdirectory (git-import)
add_subdirectory (install)
add_subdirectory (keeper-bench)
add_subdirectory (keeper-data-dumper)
add_subdirectory (keeper-utils)
add_subdirectory (local)
add_subdirectory (obfuscator)
add_subdirectory (server)
add_subdirectory (static-files-disk-uploader)
add_subdirectory (su)
add_subdirectory (zookeeper-dump-tree)
add_subdirectory (zookeeper-remove-by-list)

if (ENABLE_CLICKHOUSE_KEEPER_CONVERTER)
    add_subdirectory (keeper-converter)
endif()

if (ENABLE_CLICKHOUSE_KEEPER_CLIENT)
    add_subdirectory (keeper-client)
endif()

if (ENABLE_CLICKHOUSE_KEEPER)
    add_subdirectory (keeper)
endif()

if (ENABLE_CLICKHOUSE_SELF_EXTRACTING AND NOT ENABLE_DUMMY_LAUNCHERS)
    add_subdirectory (self-extracting)
endif ()

clickhouse_add_executable (clickhouse main.cpp)

# A library that prevent usage of several functions from libc.
if (ARCH_AMD64 AND OS_LINUX AND NOT OS_ANDROID)
    set (HARMFUL_LIB harmful)
endif ()

target_link_libraries (clickhouse PRIVATE clickhouse_common_io ${HARMFUL_LIB})
target_include_directories (clickhouse PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

if (ENABLE_CLICKHOUSE_KEEPER)
    clickhouse_target_link_split_lib(clickhouse keeper)
endif()
if (ENABLE_CLICKHOUSE_KEEPER_CONVERTER)
    clickhouse_target_link_split_lib(clickhouse keeper-converter)
endif()
if (ENABLE_CLICKHOUSE_KEEPER_CLIENT)
    clickhouse_target_link_split_lib(clickhouse keeper-client)
endif()
clickhouse_target_link_split_lib(clickhouse install)

macro(clickhouse_program_alias alias)
    message(STATUS "Adding alias ${alias}")
    add_custom_command (TARGET clickhouse POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink clickhouse ${alias})
    install (FILES "${CMAKE_CURRENT_BINARY_DIR}/${alias}" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT clickhouse)
endmacro()

set (CLICKHOUSE_BUNDLE)
function(clickhouse_program_install name lib_name)
    clickhouse_target_link_split_lib(clickhouse ${lib_name})
    add_custom_command (TARGET clickhouse POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink clickhouse ${name})
    install (FILES "${CMAKE_CURRENT_BINARY_DIR}/${name}" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT clickhouse)

    foreach(alias ${ARGN})
        clickhouse_program_alias(${alias})
    endforeach()
endfunction()

if (ENABLE_CLICKHOUSE_SELF_EXTRACTING)
    list(APPEND CLICKHOUSE_BUNDLE self-extracting)
endif ()

if (TARGET ch_rust::chdig)
    clickhouse_program_alias(clickhouse-chdig)
    clickhouse_program_alias(chdig)
else()
    message(STATUS "Adding dummy chdig targets (for nfpm)")
    foreach (chdig_path ${CMAKE_BINARY_DIR}/chdig ${CMAKE_BINARY_DIR}/clickhouse-chdig)
        file(GENERATE OUTPUT ${chdig_path} CONTENT "#!/usr/bin/env sh\necho chdig is not enabled in this ClickHouse build >&2 && exit 1\n")
        install(FILES ${chdig_path}
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                        GROUP_READ GROUP_EXECUTE
                        WORLD_READ WORLD_EXECUTE
        )
    endforeach()
endif ()

clickhouse_program_install(clickhouse-benchmark benchmark)
clickhouse_program_install(clickhouse-check-marks check-marks)
clickhouse_program_install(clickhouse-checksum-for-compressed-block checksum-for-compressed-block)
clickhouse_program_install(clickhouse-client client chc)
clickhouse_program_install(clickhouse-compressor compressor)
clickhouse_program_install(clickhouse-disks disks)
clickhouse_program_install(clickhouse-extract-from-config extract-from-config)
clickhouse_program_install(clickhouse-format format)
clickhouse_program_install(clickhouse-fst-dump-tree fst-dump-tree)
clickhouse_program_install(clickhouse-git-import git-import)
clickhouse_program_install(clickhouse-local local chl ch)
clickhouse_program_install(clickhouse-obfuscator obfuscator)
clickhouse_program_install(clickhouse-server server)
clickhouse_program_install(clickhouse-static-files-disk-uploader static-files-disk-uploader)
clickhouse_program_install(clickhouse-su su)
clickhouse_program_install(clickhouse-zookeeper-dump-tree zookeeper-dump-tree)
clickhouse_program_install(clickhouse-zookeeper-remove-by-list zookeeper-remove-by-list)

if (TARGET ch_contrib::nuraft)
    clickhouse_program_install(clickhouse-keeper-data-dumper keeper-data-dumper)
    clickhouse_program_install(clickhouse-keeper-utils keeper-utils)
endif ()

if (TARGET ch_contrib::rapidjson AND TARGET ch_contrib::nuraft)
    clickhouse_program_install(clickhouse-keeper-bench keeper-bench)
endif ()

if (ENABLE_CLICKHOUSE_KEEPER)
    if (NOT BUILD_STANDALONE_KEEPER)
        add_custom_command (TARGET clickhouse POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink clickhouse clickhouse-keeper)
        install (FILES "${CMAKE_CURRENT_BINARY_DIR}/clickhouse-keeper" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT clickhouse)
        clickhouse_make_empty_debug_info_for_nfpm(TARGET clickhouse BINARY clickhouse-keeper DESTINATION_DIR ${CMAKE_CURRENT_BINARY_DIR}/../${SPLIT_DEBUG_SYMBOLS_DIR})
    endif()

    # otherwise we don't build keeper
    if (BUILD_STANDALONE_KEEPER)
        list(APPEND CLICKHOUSE_BUNDLE clickhouse-keeper)
    endif()
endif ()
if (ENABLE_CLICKHOUSE_KEEPER_CONVERTER)
    add_custom_command (TARGET clickhouse POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink clickhouse clickhouse-keeper-converter)
    install (FILES "${CMAKE_CURRENT_BINARY_DIR}/clickhouse-keeper-converter" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT clickhouse)
endif ()
if (ENABLE_CLICKHOUSE_KEEPER_CLIENT)
    if (NOT BUILD_STANDALONE_KEEPER)
        add_custom_command (TARGET clickhouse POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink clickhouse clickhouse-keeper-client)
        install (FILES "${CMAKE_CURRENT_BINARY_DIR}/clickhouse-keeper-client" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT clickhouse)
    endif ()
endif ()

add_custom_target (clickhouse-bundle ALL DEPENDS ${CLICKHOUSE_BUNDLE})

if (USE_BINARY_HASH)
    add_custom_command(TARGET clickhouse POST_BUILD COMMAND ./clickhouse hash-binary > hash && ${OBJCOPY_PATH} --add-section .clickhouse.hash=hash clickhouse COMMENT "Adding section '.clickhouse.hash' to clickhouse binary" VERBATIM)
endif()

if (CHECK_LARGE_OBJECT_SIZES)
    add_custom_command(TARGET clickhouse POST_BUILD
        COMMAND "${CMAKE_SOURCE_DIR}/utils/check-large-objects.sh" "${CMAKE_BINARY_DIR}")
endif ()

if (SPLIT_DEBUG_SYMBOLS)
    clickhouse_split_debug_symbols(TARGET clickhouse DESTINATION_DIR ${CMAKE_CURRENT_BINARY_DIR}/${SPLIT_DEBUG_SYMBOLS_DIR} BINARY_PATH clickhouse)
else()
    clickhouse_make_empty_debug_info_for_nfpm(TARGET clickhouse BINARY clickhouse DESTINATION_DIR ${CMAKE_CURRENT_BINARY_DIR}/${SPLIT_DEBUG_SYMBOLS_DIR})
    install (TARGETS clickhouse RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT clickhouse)
endif()

if (BUILD_STRIPPED_BINARY)
    add_custom_target(clickhouse-stripped ALL
        COMMAND "${STRIP_PATH}" -o "${CMAKE_CURRENT_BINARY_DIR}/clickhouse-stripped" --strip-debug --remove-section=.comment --remove-section=.note "${CMAKE_CURRENT_BINARY_DIR}/clickhouse"
        DEPENDS clickhouse
        COMMENT "Stripping clickhouse binary" VERBATIM)
endif()

if (ENABLE_TESTS)
    set (CLICKHOUSE_UNIT_TESTS_TARGETS unit_tests_dbms)
    add_custom_target (clickhouse-tests ALL DEPENDS ${CLICKHOUSE_UNIT_TESTS_TARGETS})
    add_dependencies(clickhouse-bundle clickhouse-tests)
endif()

if (TARGET ch_contrib::protobuf)
    get_property(google_proto_files TARGET ch_contrib::protobuf PROPERTY google_proto_files)
    foreach (proto_file IN LISTS google_proto_files)
        install(FILES ${proto_file} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/clickhouse/protos/google/protobuf)
    endforeach()
endif ()
