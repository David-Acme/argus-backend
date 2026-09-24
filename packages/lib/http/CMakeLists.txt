cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../errors
                     ${CMAKE_BINARY_DIR}/errors)
endif()

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()

if(NOT TARGET argus::lib::mdns)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mdns
                     ${CMAKE_BINARY_DIR}/mdns)
endif()

if(NOT TARGET argus::contracts::routes)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/routes
                     ${CMAKE_BINARY_DIR}/contracts-routes)
endif()

argus_lib(NAME http
    SOURCES
        src/http/api-response.cc
        src/http/cors.cc
        src/http/error-handler.cc
        src/http/health-controller.cc
        src/http/listener-config.cc
        src/http/logical-routes.cc
        src/http/route-announcements.cc
        src/http/api-response.hxx
        src/http/cors.hxx
        src/http/error-handler.hxx
        src/http/health-controller.hxx
        src/http/details/http-errors.hxx
        src/http/listener-config.hxx
        src/http/logical-routes.hxx
        src/http/route-announcements.hxx
    INCLUDES src
    DEPENDS
        argus::lib::errors
        argus::lib::config
        argus::lib::mdns
        argus::contracts::routes
        Drogon::Drogon)

target_compile_options(argus_lib_http PRIVATE -Wall -Wextra)

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

add_executable(logical-routes-test tests/unit/logical-routes-test.cc)
target_link_libraries(logical-routes-test PRIVATE argus::lib::http doctest::doctest)
target_compile_options(logical-routes-test PRIVATE -Wall -Wextra)

add_test(NAME logical-routes-test COMMAND logical-routes-test)
