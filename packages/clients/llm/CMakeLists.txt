# argus_clients_llm: the argus-llm wire seen from the caller's side — the HTTP
# client and the chat contract it speaks. argus-memory and argus-voice both
# stream completions through this; argus-llm links it for the contract, the
# same shape tts-client has for the TTS wire.
#
# It also carries tool-contracts.hxx, the vocabulary a tool descriptor is
# written in. That header needs a tier argus-memory and argus-llm may both
# reach — memory declares the descriptors, llm runs them, and a package may not
# include a service — so it waits here for the move that takes memory into
# argus-llm (Phase 4 step 7). The machinery that turns a call into a handler
# run (registry, validator, executor) had one consumer and left with Phase 1
# step 9 (D16).

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/auth
                     ${CMAKE_CURRENT_BINARY_DIR}/auth)
endif()

if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_clients(NAME llm
    SOURCES
        src/llm/details/llm-remote.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::auth argus::contracts::auth argus::contracts::sync argus::lib::config
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_clients_llm PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(llm-client-test tests/unit/llm-client-test.cc)

# This folder is pulled in EXCLUDE_FROM_ALL (consumers add it for the client
# targets), so the suite has to opt back into the default build to be present
# when ctest runs it.
set_target_properties(llm-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(llm-client-test PRIVATE argus::clients::llm
    doctest::doctest)

target_include_directories(llm-client-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_compile_options(llm-client-test PRIVATE -Wall -Wextra)

add_test(NAME llm-client-test COMMAND llm-client-test)
