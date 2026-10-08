set(ARGUS_LIVEKIT_SDK_VERSION 1.12.0)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(livekit_platform linux-arm64)
else()
    set(livekit_platform linux-x64)
endif()
set(livekit_archive livekit-sdk-${livekit_platform}-${ARGUS_LIVEKIT_SDK_VERSION}.tar.gz)
set(ARGUS_LIVEKIT_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/livekit-sdk
    CACHE PATH "Where the pinned LiveKit C++ SDK archives are unpacked")
cmake_path(NORMAL_PATH ARGUS_LIVEKIT_SDK_ROOT OUTPUT_VARIABLE livekit_root)
set(livekit_dir ${livekit_root}/livekit-sdk-${livekit_platform}-${ARGUS_LIVEKIT_SDK_VERSION})

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/livekit-sdk.sha256 livekit_pins)
set(livekit_sha "")
foreach(pin IN LISTS livekit_pins)
    if(pin MATCHES "^([0-9a-f]+)  ${livekit_archive}$")
        set(livekit_sha ${CMAKE_MATCH_1})
    endif()
endforeach()
if(NOT livekit_sha)
    message(FATAL_ERROR "No SHA-256 pin for ${livekit_archive} in livekit-sdk.sha256")
endif()

if(NOT EXISTS ${livekit_dir}/lib/cmake/LiveKit/LiveKitConfig.cmake)
    file(MAKE_DIRECTORY ${livekit_root})
    file(LOCK ${livekit_root}/.lock GUARD PROCESS TIMEOUT 600)
    if(NOT EXISTS ${livekit_dir}/lib/cmake/LiveKit/LiveKitConfig.cmake)
        file(DOWNLOAD
            https://github.com/livekit/client-sdk-cpp/releases/download/v${ARGUS_LIVEKIT_SDK_VERSION}/${livekit_archive}
            ${livekit_root}/${livekit_archive}
            EXPECTED_HASH SHA256=${livekit_sha}
            TLS_VERIFY ON
            STATUS livekit_status)
        list(GET livekit_status 0 livekit_code)
        if(NOT livekit_code EQUAL 0)
            message(FATAL_ERROR "Downloading ${livekit_archive} failed: ${livekit_status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT ${livekit_root}/${livekit_archive} DESTINATION ${livekit_root})
        file(REMOVE ${livekit_root}/${livekit_archive})
    endif()
    file(LOCK ${livekit_root}/.lock RELEASE)
endif()

find_package(LiveKit ${ARGUS_LIVEKIT_SDK_VERSION} EXACT CONFIG REQUIRED
             PATHS ${livekit_dir} NO_DEFAULT_PATH)

set_property(GLOBAL APPEND PROPERTY ARGUS_RPATH_DIRS ${livekit_dir}/lib)
set(ARGUS_LIVEKIT_SDK_LIBS
    ${livekit_dir}/lib/liblivekit.so
    ${livekit_dir}/lib/liblivekit_ffi.so
    PARENT_SCOPE)

argus_module(NAME voice-rtc-wire
    SOURCES
        rtc-wire.cc
        playout-gain.cc
    INCLUDES
        ../..
    DEPENDS
        argus::voice-core)

target_compile_options(argus_voice-rtc-wire PRIVATE -Wall -Wextra)

argus_module(NAME voice-rtc
    SOURCES
        rtc-call.cc
        rtc-agent-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::voice-rtc-wire
        argus::voice-core
        argus::clients::notification
    SYSTEM_DEPENDS
        LiveKit::livekit)

target_compile_options(argus_voice-rtc PRIVATE -Wall -Wextra)
