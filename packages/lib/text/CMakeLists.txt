include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME text
    SOURCES
        src/text/base64.cc
        src/text/base64.hxx
        src/text/fnv-hash.hxx
        src/text/json-diff.cc
        src/text/json-diff.hxx
        src/text/json-util.hxx
        src/text/sha256.hxx
        src/text/text-norm.cc
        src/text/text-norm.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_lib_text PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(sha256-test tests/unit/sha256-test.cc)
target_link_libraries(sha256-test PRIVATE argus::lib::text doctest::doctest)
target_compile_options(sha256-test PRIVATE -Wall -Wextra)
add_test(NAME sha256-test COMMAND sha256-test)

add_executable(json-diff-test tests/unit/json-diff-test.cc)
target_link_libraries(json-diff-test PRIVATE argus::lib::text doctest::doctest)
target_compile_options(json-diff-test PRIVATE -Wall -Wextra)
add_test(NAME json-diff-test COMMAND json-diff-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
