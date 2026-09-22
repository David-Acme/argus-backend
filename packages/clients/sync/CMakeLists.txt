cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

# The frame this leg carries is contracts/sync's vocabulary -- the C++ types
# (SocketEmitDto, sync_change::RoleRoomChange) and the proto family the frame
# comes from -- so the contract is guarded in, the way clients/llm guards it.
# The guard also is what defines argus::lib::text, which the translation unit
# uses to put the row on the wire as JSON.
if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

if(NOT TARGET argus_client_grpc_base)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/grpc
                     ${CMAKE_CURRENT_BINARY_DIR}/grpc)
endif()

# Two protos, not one: the control schema imports contracts.proto for
# SyncFrame, and the macro generates per file, so the imported file has to be
# compiled here too or the frame's symbols are missing at compile time. This is
# the only CMakeLists that compiles contracts.proto -- the contract package is
# an INTERFACE target and ships no translation unit.
if(NOT TARGET argus::clients::sync)
  argus_clients(NAME sync
      PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/proto
      PROTO argus/sync/v1/contracts.proto
            argus/sync/v1/sync.proto
      SOURCES src/sync/sync-client.cc
      INCLUDES src
      DEPENDS
          argus::contracts::sync
          argus::lib::text)
endif()

enable_testing()
find_package(doctest REQUIRED)

# The suite: guarded like the client above, so a second consumer pulling this
# folder in still registers exactly one executable and one test.
if(NOT TARGET sync-client-test)
  add_executable(sync-client-test tests/unit/sync-client-test.cc)

  # This folder is pulled in EXCLUDE_FROM_ALL (consumers add it for the client
  # target), so the suite has to opt back into the default build to be present
  # when ctest runs it.
  set_target_properties(sync-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

  target_link_libraries(sync-client-test PRIVATE argus::clients::sync
      doctest::doctest)

  target_compile_options(sync-client-test PRIVATE -Wall -Wextra)

  add_test(NAME sync-client-test COMMAND sync-client-test)
endif()
