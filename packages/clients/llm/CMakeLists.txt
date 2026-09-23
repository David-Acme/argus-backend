include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

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
        argus::contracts::sync argus::lib::config
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_clients_llm PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(llm-client-test tests/unit/llm-client-test.cc)

set_target_properties(llm-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(llm-client-test PRIVATE argus::clients::llm
    doctest::doctest)

target_include_directories(llm-client-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_compile_options(llm-client-test PRIVATE -Wall -Wextra)

add_test(NAME llm-client-test COMMAND llm-client-test)
