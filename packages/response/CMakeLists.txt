cmake_minimum_required(VERSION 3.25)
project(response VERSION 1.0.0 LANGUAGES CXX)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

add_library(response STATIC src/response-exception.cc src/config/app-config.cc
    src/http/api-response.cc)
add_library(pkg::response ALIAS response)
target_compile_features(response PUBLIC cxx_std_20)
set_target_properties(response PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(response PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(response PUBLIC Drogon::Drogon)
target_compile_options(response PRIVATE -Wall -Wextra)
