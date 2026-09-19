cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

if(NOT TARGET argus::client-identity)
  argus_client_module(NAME identity
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/identity/v1/identity.proto
      SOURCES src/identity/identity-client.cc
      INCLUDES src)
endif()
