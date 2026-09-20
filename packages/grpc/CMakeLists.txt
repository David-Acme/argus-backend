cmake_minimum_required(VERSION 3.25)

# The reusable gRPC runtime every generated client and every RPC server
# shares: channel/credential/metadata helpers, the vendored-stack ABI bridge,
# the server-side caller-identity checks and the standard grpc.health.v1
# stubs. Consumers link the generated argus::client-<domain> modules, which
# pull this in; the health stubs are the exception -- they are served, not
# called, so a service links argus::grpc-health directly.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

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
# generated-client SDK. GROUP grpc names them for what they are
# (argus_grpc_health / argus::grpc-health) instead of inheriting the client
# convention; the codegen and the link come from the same helper as every
# generated client. EXCLUDE_FROM_ALL keeps the stubs out of projects that pull
# this package in for the runtime alone.
if(NOT TARGET argus::grpc-health)
  argus_client_module(NAME health
      GROUP grpc
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/proto
      PROTO grpc/health/v1/health.proto)
  set_target_properties(argus_grpc_health PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
