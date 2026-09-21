cmake_minimum_required(VERSION 3.25)

# The reusable gRPC runtime every generated client and every RPC server
# shares: channel/credential/metadata helpers, the vendored-stack ABI bridge,
# the server-side caller-identity checks and the standard grpc.health.v1
# stubs. Consumers link the generated argus::clients::<domain> modules, which
# pull this in; the health stubs are the exception -- they are served, not
# called, so a service links argus::lib::grpc-health directly.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_contracts_substrate()

# Directory-scope ensure: the module custom commands evaluate
# $<TARGET_FILE*:gRPC::grpc*> at generate time, so the imported targets must
# exist in directory scope, not just inside the substrate function.
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

# The standard grpc.health.v1 stubs. They were packages/clients/health, a
# proto-only package with zero sources: every service that answers health
# probes implements grpc::health::v1::Health::CallbackService over them, and
# nothing calls them, so they are part of the gRPC runtime rather than of the
# generated-client SDK. They are the one wire module that belongs to no domain,
# so they pass the group they live in (lib/grpc) and are named for what they
# are -- the gRPC runtime's health stubs; the codegen and the link come from the
# same helper as every generated client. EXCLUDE_FROM_ALL keeps the stubs out of
# projects that pull this package in for the runtime alone.
if(NOT TARGET argus::lib::grpc-health)
  argus_client_module(NAME grpc-health
      GROUP lib
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO grpc/health/v1/health.proto)
  set_target_properties(argus_lib_grpc-health PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# The receiving side's own suite: secret comparison and the credential set a
# server authorizes against. Both are pure, so they need no channel -- the
# base target is linked for the include root and the gRPC headers it publishes.
enable_testing()
find_package(doctest REQUIRED)

add_executable(grpc-server-identity-test tests/unit/grpc-server-identity-test.cc)

# This folder is pulled in EXCLUDE_FROM_ALL (consumers add the client that
# needs it for the client target), so the suite has to opt back into the
# default build to be present when ctest runs it.
set_target_properties(grpc-server-identity-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(grpc-server-identity-test PRIVATE
    argus_client_grpc_base
    doctest::doctest)
target_compile_options(grpc-server-identity-test PRIVATE -Wall -Wextra)
add_test(NAME grpc-server-identity-test COMMAND grpc-server-identity-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
