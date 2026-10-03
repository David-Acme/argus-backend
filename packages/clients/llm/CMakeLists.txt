include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
if(NOT TARGET argus::contracts::llm)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/llm
                     ${CMAKE_BINARY_DIR}/contracts/llm)
endif()
argus_llm_rpc_contract()

argus_clients(NAME llm
    SOURCES
        src/llm/llm-client.cc
        src/llm/llm-remote.cc
    INCLUDES
        src
    DEPENDS
        argus::contracts::auth argus::contracts::sync argus::lib::config
        argus::lib::errors
        argus::contracts::llm
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_link_libraries(argus_clients_llm PRIVATE argus::contracts::llm-wire)
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
