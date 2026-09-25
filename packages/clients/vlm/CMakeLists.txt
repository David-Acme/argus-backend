include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

if(NOT TARGET argus::lib::runtime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/runtime
                     ${CMAKE_CURRENT_BINARY_DIR}/runtime)
endif()

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/text
                     ${CMAKE_CURRENT_BINARY_DIR}/text)
endif()

find_package(Drogon CONFIG REQUIRED)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
if(NOT TARGET argus::contracts::vlm)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/vlm
                     ${CMAKE_BINARY_DIR}/contracts/vlm)
endif()
argus_vlm_rpc_contract()

argus_clients(NAME vlm
    SOURCES
        src/vlm/vlm-client.cc
        src/vlm/vlm-remote.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::config argus::lib::errors
        argus::lib::runtime argus::lib::text
        argus::contracts::vlm
        Drogon::Drogon)

target_link_libraries(argus_clients_vlm PRIVATE argus::contracts::vlm-wire)
target_compile_options(argus_clients_vlm PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(vlm-client-test tests/unit/vlm-client-test.cc)

set_target_properties(vlm-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(vlm-client-test PRIVATE argus::clients::vlm
    doctest::doctest)

target_include_directories(vlm-client-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_compile_options(vlm-client-test PRIVATE -Wall -Wextra)

add_test(NAME vlm-client-test COMMAND vlm-client-test)
