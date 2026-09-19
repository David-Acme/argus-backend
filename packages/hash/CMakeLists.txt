# hash: hashing and encoding utilities (SHA-256, FNV-1a, base64). No
# dependencies beyond the standard library; consumers link argus::hash.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_module(NAME hash
    SOURCES
        src/shared/utils/base64/base64.cc
        src/shared/utils/base64/base64.hxx
        src/shared/utils/fnv-hash/fnv-hash.hxx
        src/shared/utils/sha256/sha256.hxx
    INCLUDES src)

target_compile_options(argus_hash PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(sha256-test tests/unit/sha256-test.cc)
target_link_libraries(sha256-test PRIVATE argus_hash doctest::doctest)
target_compile_options(sha256-test PRIVATE -Wall -Wextra)
add_test(NAME sha256-test COMMAND sha256-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
