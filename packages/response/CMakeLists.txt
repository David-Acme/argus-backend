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

# The envelope suite: the {status, info, errors} formatter, the exception
# vocabulary and the advice every service registers. Nothing here reaches the
# wire layer, so this package configures without protobuf.
enable_testing()
find_package(doctest REQUIRED)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(api-response-test tests/unit/api-response-test.cc)
target_link_libraries(api-response-test PRIVATE pkg::response doctest::doctest)
target_compile_options(api-response-test PRIVATE -Wall -Wextra)

add_test(NAME api-response-test COMMAND api-response-test)
