cmake_minimum_required(VERSION 3.25)

# The reusable gRPC runtime every generated client and every RPC server
# shares: channel/credential/metadata helpers, the vendored-stack ABI bridge
# and the server-side caller-identity checks. Consumers link the generated
# argus::client-<domain> modules (which pull this in); nobody links this
# package's objects directly except through them.

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
