include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_lib(NAME net
    SOURCES
        src/net/loopback-socket.cc
        src/net/loopback-socket.hxx
    INCLUDES src)

target_compile_options(argus_lib_net PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(loopback-socket-test tests/unit/loopback-socket-test.cc)
target_link_libraries(loopback-socket-test PRIVATE argus::lib::net doctest::doctest)
target_compile_options(loopback-socket-test PRIVATE -Wall -Wextra)
add_test(NAME loopback-socket-test COMMAND loopback-socket-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
