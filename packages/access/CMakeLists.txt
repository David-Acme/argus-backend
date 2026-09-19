# access: shared vocabulary (enums) plus the single role-permission table both
# the HTTP RoleFilter and the sync engine read. Header-only; consumers link
# argus::access.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_module(NAME access
    SOURCES
        src/shared/enums.hxx
        src/shared/access/role-access.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

# Header-only: no compiled source sets the linker language on its own.
set_target_properties(argus_access PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(argus_access PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

foreach(TEST_NAME IN ITEMS enums-test role-access-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    target_link_libraries(${TEST_NAME} PRIVATE argus_access doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
