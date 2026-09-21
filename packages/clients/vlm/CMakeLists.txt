# argus_clients_vlm: the argus-vlm internal wire seen from the caller's side.
# argus-guard is the only consumer; the client owns the envelope and the
# base64 request encoding so no service hand-rolls the wire (rule 23).

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/text
                     ${CMAKE_CURRENT_BINARY_DIR}/text)
endif()

find_package(Drogon CONFIG REQUIRED)

argus_clients(NAME vlm
    SOURCES
        src/vlm/vlm-client.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_clients_vlm PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(vlm-client-test tests/unit/vlm-client-test.cc)

# This folder is pulled in EXCLUDE_FROM_ALL (consumers add it for the client
# targets), so the suite has to opt back into the default build to be present
# when ctest runs it.
set_target_properties(vlm-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(vlm-client-test PRIVATE argus::clients::vlm
    doctest::doctest)

target_compile_options(vlm-client-test PRIVATE -Wall -Wextra)

add_test(NAME vlm-client-test COMMAND vlm-client-test)
