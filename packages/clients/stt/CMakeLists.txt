# argus_clients_stt: the argus-stt wire seen from the caller's side. The header
# IS the contract (16 kHz mono s16 PCM in, transcript out), so argus-stt
# includes it as well — the same shape tts-client and llm-client
# have for their wires.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

find_package(Drogon CONFIG REQUIRED)

argus_clients(NAME stt
    SOURCES
        src/stt/stt-remote.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::config
        Drogon::Drogon)

target_compile_options(argus_clients_stt PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(stt-client-test tests/unit/stt-client-test.cc)

# This folder is pulled in EXCLUDE_FROM_ALL (consumers add it for the client
# targets), so the suite has to opt back into the default build to be present
# when ctest runs it.
set_target_properties(stt-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(stt-client-test PRIVATE argus::clients::stt
    doctest::doctest)

target_include_directories(stt-client-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_compile_options(stt-client-test PRIVATE -Wall -Wextra)

add_test(NAME stt-client-test COMMAND stt-client-test)
