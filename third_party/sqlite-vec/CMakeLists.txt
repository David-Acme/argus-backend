cmake_minimum_required(VERSION 3.25)
project(sqlite-vec LANGUAGES C)

add_library(sqlite-vec STATIC sqlite-vec.c)
target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
target_include_directories(sqlite-vec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
