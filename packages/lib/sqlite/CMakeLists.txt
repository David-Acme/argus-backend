cmake_minimum_required(VERSION 3.25)
project(argus-sqlite VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# argus_lib_sqlite: the SQLite substrate — DbService boot, VecDb, the vec0
# vector-index repository and the small SQL helpers (prepared-statement RAII,
# escaping, schema runner). Consumers link argus::lib::sqlite.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

# Found before the sibling add_subdirectory block: argus_lib_sqlite links Drogon
# from this scope.
find_package(Drogon CONFIG REQUIRED)

# schema-runner compiles sqlite3.h directly; a transitive-only conan entry
# generates empty include dirs.
if(NOT TARGET SQLite::SQLite3)
    find_package(SQLite CONFIG REQUIRED)
endif()

# The vendored sqlite-vec the vec repositories ride and the sibling package the
# module links, declared here rather than by the consumer: the guards keep the
# root tree and every other consumer in charge.
if(NOT TARGET sqlite-vec)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/sqlite-vec
                     ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
endif()
set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()

argus_lib(NAME sqlite
    SOURCES
        src/shared/services/sqlite/db-service.cc
        src/shared/services/sqlite/vec-db.cc
        src/shared/repositories/vector-index/vector-index-repository.cc
        src/shared/wrapper/sqlite-stmt/sqlite-stmt.hxx
        src/shared/utils/sql-escape/sql-escape.hxx
        src/shared/utils/schema-runner/schema-runner.cc
        src/shared/utils/schema-runner/schema-runner.hxx
    INCLUDES src)

target_link_libraries(argus_lib_sqlite PUBLIC argus::lib::config Drogon::Drogon)

if(TARGET SQLite::SQLite3)
    target_link_libraries(argus_lib_sqlite PRIVATE SQLite::SQLite3)
endif()

if(ARGUS_SQLITE_VEC_TARGET AND TARGET ${ARGUS_SQLITE_VEC_TARGET})
    target_link_libraries(argus_lib_sqlite PUBLIC ${ARGUS_SQLITE_VEC_TARGET})
endif()

target_compile_options(argus_lib_sqlite PRIVATE -Wall -Wextra)

# The named identity client slot: installed at boot, falling back to the
# default client when the host installs none.
find_package(doctest REQUIRED)

add_executable(identity-client-test tests/unit/identity-client-test.cc)
target_link_libraries(identity-client-test PRIVATE
    argus::lib::sqlite
    argus::lib::config
    Drogon::Drogon
    sqlite-vec
    doctest::doctest)
target_compile_options(identity-client-test PRIVATE -Wall -Wextra)
add_test(NAME identity-client-test COMMAND identity-client-test)
