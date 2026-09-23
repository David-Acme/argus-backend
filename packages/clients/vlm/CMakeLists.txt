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

set_target_properties(vlm-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(vlm-client-test PRIVATE argus::clients::vlm
    doctest::doctest)

target_compile_options(vlm-client-test PRIVATE -Wall -Wextra)

add_test(NAME vlm-client-test COMMAND vlm-client-test)
