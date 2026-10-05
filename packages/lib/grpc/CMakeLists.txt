cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_contracts_substrate()

find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
  list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local/argus-thirdparty/grpc")
  find_package(gRPC CONFIG REQUIRED)
endif()

if(NOT TARGET argus_client_grpc_base)
  argus_grpc_client_base()
endif()

if(NOT TARGET argus_client_grpc_bridge_entry)
  argus_grpc_absl_bridge()
endif()

if(NOT TARGET argus::lib::grpc-health)
  argus_client_module(NAME grpc-health
      GROUP lib
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO grpc/health/v1/health.proto)
  set_target_properties(argus_lib_grpc-health PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(grpc-server-identity-test tests/unit/grpc-server-identity-test.cc)

set_target_properties(grpc-server-identity-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(grpc-server-identity-test PRIVATE
    argus_client_grpc_base
    doctest::doctest)
target_compile_options(grpc-server-identity-test PRIVATE -Wall -Wextra)
add_test(NAME grpc-server-identity-test COMMAND grpc-server-identity-test)

add_executable(fleet-caller-gate-test tests/unit/fleet-caller-gate-test.cc)

set_target_properties(fleet-caller-gate-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(fleet-caller-gate-test PRIVATE
    argus_client_grpc_base
    doctest::doctest)
target_compile_options(fleet-caller-gate-test PRIVATE -Wall -Wextra)
add_test(NAME fleet-caller-gate-test COMMAND fleet-caller-gate-test)

add_executable(grpc-server-drain-test tests/unit/grpc-server-drain-test.cc)

set_target_properties(grpc-server-drain-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(grpc-server-drain-test PRIVATE
    argus::lib::grpc-health
    doctest::doctest)
target_compile_options(grpc-server-drain-test PRIVATE -Wall -Wextra)
add_test(NAME grpc-server-drain-test COMMAND grpc-server-drain-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
