cmake_minimum_required(VERSION 3.25)

# http: the substrate every service answers through -- the {status, info,
# errors} envelope, the one advice that turns a refusal into it, the CORS
# headers, the shared health controller and the listener resolution. It owns no
# domain vocabulary: the codes it answers with come from argus::lib::errors, and the
# services it boots sit on argus::lib::config. Consumers link argus::lib::http.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../errors
                     ${CMAKE_BINARY_DIR}/errors)
endif()

# The listener resolution reads the loaded TOML through ConfigService, so the
# substrate sits on top of the config package. The edge is a DAG: config boots
# a service and answers nothing over HTTP.
if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()

argus_lib(NAME http
    SOURCES
        src/http/api-response.cc
        src/http/cors.cc
        src/http/error-handler.cc
        src/http/health-controller.cc
        src/http/listener-config.cc
        src/http/api-response.hxx
        src/http/cors.hxx
        src/http/error-handler.hxx
        src/http/health-controller.hxx
        src/http/http-errors.hxx
        src/http/listener-config.hxx
    INCLUDES src
    DEPENDS
        argus::lib::errors
        argus::lib::config
        Drogon::Drogon)

target_compile_options(argus_lib_http PRIVATE -Wall -Wextra)

# The substrate's own suite: the envelope's four shapes, the definitions it
# answers with, the advice, and CORS. Nothing here reaches the wire layer, so
# this package configures without protobuf.
enable_testing()
find_package(doctest REQUIRED)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(api-response-test tests/unit/api-response-test.cc)
target_link_libraries(api-response-test PRIVATE argus::lib::http doctest::doctest)
target_compile_options(api-response-test PRIVATE -Wall -Wextra)

add_test(NAME api-response-test COMMAND api-response-test)
