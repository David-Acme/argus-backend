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
        src/text/iso-time.cc
        src/text/iso-time.hxx
        src/text/json-util.hxx
        src/text/name-match.cc
        src/text/name-match.hxx
        src/text/sha256.hxx
        src/text/spoken-time.cc
        src/text/spoken-time.hxx
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

add_executable(iso-time-test tests/unit/iso-time-test.cc)
target_link_libraries(iso-time-test PRIVATE argus::lib::text doctest::doctest)
target_compile_options(iso-time-test PRIVATE -Wall -Wextra)
add_test(NAME iso-time-test COMMAND iso-time-test)

add_executable(name-match-test tests/unit/name-match-test.cc)
target_link_libraries(name-match-test PRIVATE argus::lib::text doctest::doctest)
target_compile_options(name-match-test PRIVATE -Wall -Wextra)
add_test(NAME name-match-test COMMAND name-match-test)

add_executable(spoken-time-test tests/unit/spoken-time-test.cc)
target_link_libraries(spoken-time-test PRIVATE argus::lib::text doctest::doctest)
target_compile_options(spoken-time-test PRIVATE -Wall -Wextra)
add_test(NAME spoken-time-test COMMAND spoken-time-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
