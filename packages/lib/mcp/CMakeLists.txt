include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME mcp
    SOURCES
        src/mcp/client.cc
        src/mcp/client.hxx
        src/mcp/confirmation.cc
        src/mcp/confirmation.hxx
        src/mcp/json-rpc.cc
        src/mcp/json-rpc.hxx
        src/mcp/local-transport.hxx
        src/mcp/loop-tool.cc
        src/mcp/loop-tool.hxx
        src/mcp/schema.cc
        src/mcp/schema.hxx
        src/mcp/server.cc
        src/mcp/server.hxx
        src/mcp/speech.cc
        src/mcp/speech.hxx
        src/mcp/tool.cc
        src/mcp/tool.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_lib_mcp PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

foreach(test_name json-rpc-test schema-test tool-test speech-test confirmation-test loop-tool-test mcp-server-test mcp-client-test)
    add_executable(${test_name} tests/unit/${test_name}.cc)
    target_link_libraries(${test_name} PRIVATE argus::lib::mcp doctest::doctest)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
