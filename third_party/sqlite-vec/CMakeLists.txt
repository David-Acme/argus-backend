cmake_minimum_required(VERSION 3.25)
project(sqlite-vec LANGUAGES C)

# vec0 vector search for SQLite (vendored, MIT/Apache-2.0). Compiled with
# SQLITE_CORE so it links statically against Drogon's sqlite3 (it only
# defines vec0_*/sqlite3_vec_init symbols — no clash).
add_library(sqlite-vec STATIC sqlite-vec.c)
target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
target_include_directories(sqlite-vec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
