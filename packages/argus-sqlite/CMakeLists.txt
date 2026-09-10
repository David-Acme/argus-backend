cmake_minimum_required(VERSION 3.25)
project(argus-sqlite VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_sqlite: the SQLite substrate — DbService boot, VecDb and the vec0
# vector-index repository. Consumers link argus::sqlite.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# Found before the sibling add_subdirectory block: argus_sqlite links Drogon
# from this scope.
find_package(Drogon CONFIG REQUIRED)

# Standalone configure (top level): the sqlite module links argus_common and
# the vendored sqlite-vec the vec repositories ride. Every guard keeps the
# root and consumer builds in charge.
if(PROJECT_IS_TOP_LEVEL)
    if(NOT TARGET argus_common)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-common
                         ${CMAKE_BINARY_DIR}/argus-common)
    endif()
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)
endif()

argus_module(NAME sqlite
    SOURCES
        src/shared/services/sqlite/db-service.cc
        src/shared/services/sqlite/vec-db.cc
        src/shared/repositories/vector-index/vector-index-repository.cc
    INCLUDES src)

target_link_libraries(argus_sqlite
    PUBLIC
        argus_common
        Drogon::Drogon)

if(ARGUS_SQLITE_VEC_TARGET AND TARGET ${ARGUS_SQLITE_VEC_TARGET})
    target_link_libraries(argus_sqlite PUBLIC ${ARGUS_SQLITE_VEC_TARGET})
endif()

target_compile_options(argus_sqlite PRIVATE -Wall -Wextra)

# The named identity client slot: installed at boot, falling back to the
# default client when the host installs none.
enable_testing()
find_package(doctest REQUIRED)

add_executable(identity-client-test tests/unit/identity-client-test.cc)
target_link_libraries(identity-client-test PRIVATE argus::sqlite
    argus_common Drogon::Drogon sqlite-vec doctest::doctest)
target_compile_options(identity-client-test PRIVATE -Wall -Wextra)
add_test(NAME identity-client-test COMMAND identity-client-test)
