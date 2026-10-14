include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

if(NOT TARGET argus::lib::net)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/net
                     ${CMAKE_CURRENT_BINARY_DIR}/net)
endif()

find_package(Drogon CONFIG REQUIRED)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
if(NOT TARGET argus::contracts::stt)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/stt
                     ${CMAKE_BINARY_DIR}/contracts/stt)
endif()
argus_stt_rpc_contract()

argus_clients(NAME stt
    SOURCES
        src/stt/stt-client.cc
        src/stt/stt-remote.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::config argus::lib::errors
        argus::contracts::stt
        Drogon::Drogon)

target_link_libraries(argus_clients_stt PRIVATE argus::contracts::stt-wire
    argus::lib::net)
target_compile_options(argus_clients_stt PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(stt-client-test tests/unit/stt-client-test.cc)

set_target_properties(stt-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(stt-client-test PRIVATE argus::clients::stt
    doctest::doctest)

target_include_directories(stt-client-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_compile_options(stt-client-test PRIVATE -Wall -Wextra)

add_test(NAME stt-client-test COMMAND stt-client-test)

add_executable(stt-grpc-tls-test tests/unit/stt-grpc-tls-test.cc)

set_target_properties(stt-grpc-tls-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(stt-grpc-tls-test PRIVATE argus::clients::stt
    argus::contracts::stt-wire argus::lib::grpc-test-support argus_client_grpc_base
    doctest::doctest)

target_compile_options(stt-grpc-tls-test PRIVATE -Wall -Wextra)

add_test(NAME stt-grpc-tls-test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/internal-tls-test.sh
                 $<TARGET_FILE:stt-grpc-tls-test> 1)
set_tests_properties(stt-grpc-tls-test PROPERTIES SKIP_RETURN_CODE 77)
