cmake_minimum_required(VERSION 3.25)
project(argus-sqlite VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)

if(NOT TARGET SQLite::SQLite3)
    find_package(SQLite CONFIG REQUIRED)
endif()

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
        src/sqlite/db-service.cc
        src/sqlite/vec-db.cc
        src/sqlite/details/vector-index-repository.cc
        src/sqlite/sqlite-stmt.hxx
        src/sqlite/sql-escape.hxx
        src/sqlite/schema-runner.cc
        src/sqlite/schema-runner.hxx
        src/sqlite/transaction.cc
        src/sqlite/transaction.hxx
    INCLUDES src)

target_link_libraries(argus_lib_sqlite PUBLIC argus::lib::config Drogon::Drogon)

if(TARGET SQLite::SQLite3)
    target_link_libraries(argus_lib_sqlite PRIVATE SQLite::SQLite3)
endif()

if(ARGUS_SQLITE_VEC_TARGET AND TARGET ${ARGUS_SQLITE_VEC_TARGET})
    target_link_libraries(argus_lib_sqlite PUBLIC ${ARGUS_SQLITE_VEC_TARGET})
endif()

target_compile_options(argus_lib_sqlite PRIVATE -Wall -Wextra)

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
