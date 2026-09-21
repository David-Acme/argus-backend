# text: the text and payload utilities — normalization, JSON helpers
# (parse/serialize plus flat object diffs) and hashing/encoding (SHA-256,
# FNV-1a, base64). It absorbs the json and hash packages; consumers link
# argus::lib::text.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME text
    SOURCES
        src/shared/utils/base64/base64.cc
        src/shared/utils/base64/base64.hxx
        src/shared/utils/fnv-hash/fnv-hash.hxx
        src/shared/utils/json-diff/json-diff.cc
        src/shared/utils/json-diff/json-diff.hxx
        src/shared/utils/json-util/json-util.hxx
        src/shared/utils/sha256/sha256.hxx
        src/shared/utils/text-norm/text-norm.cc
        src/shared/utils/text-norm/text-norm.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_lib_text PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(sha256-test tests/unit/sha256-test.cc)
target_link_libraries(sha256-test PRIVATE argus::lib::text doctest::doctest)
target_compile_options(sha256-test PRIVATE -Wall -Wextra)
add_test(NAME sha256-test COMMAND sha256-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
