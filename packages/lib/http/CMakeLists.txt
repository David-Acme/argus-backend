cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

find_package(OpenSSL REQUIRED)

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

if(NOT TARGET argus::lib::runtime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime
                     ${CMAKE_BINARY_DIR}/runtime)
endif()

if(NOT TARGET argus::contracts::routes)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/routes
                     ${CMAKE_BINARY_DIR}/contracts-routes)
endif()

argus_lib(NAME http
    SOURCES
        src/http/api-response.cc
        src/http/certificate-reload.cc
        src/http/cors.cc
        src/http/error-handler.cc
        src/http/health-controller.cc
        src/http/listener-config.cc
        src/http/logical-routes.cc
        src/http/route-announcements.cc
        src/http/api-response.hxx
        src/http/certificate-reload.hxx
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
        Drogon::Drogon
        OpenSSL::Crypto)

target_compile_options(argus_lib_http PRIVATE -Wall -Wextra)

argus_lib(NAME http-download
    SOURCES
        src/http/download/chunk-transport.hxx
        src/http/download/drogon-chunk-transport.cc
        src/http/download/drogon-chunk-transport.hxx
        src/http/download/file-download.cc
        src/http/download/file-download.hxx
        src/http/download/details/download-sidecar.cc
        src/http/download/details/download-sidecar.hxx
        src/http/download/details/download-url.cc
        src/http/download/details/download-url.hxx
        src/http/download/details/part-file.cc
        src/http/download/details/part-file.hxx
        src/http/download/details/peer-name.cc
        src/http/download/details/peer-name.hxx
        src/http/download/details/sha256-stream.cc
        src/http/download/details/sha256-stream.hxx
    INCLUDES src
    DEPENDS
        argus::lib::runtime
        Drogon::Drogon
        OpenSSL::Crypto)

target_compile_options(argus_lib_http-download PRIVATE -Wall -Wextra)

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

add_executable(certificate-reload-test tests/unit/certificate-reload-test.cc)
target_link_libraries(certificate-reload-test PRIVATE argus::lib::http OpenSSL::Crypto doctest::doctest)
target_compile_options(certificate-reload-test PRIVATE -Wall -Wextra)

add_test(NAME certificate-reload-test COMMAND certificate-reload-test)

add_executable(download-url-test tests/unit/download/download-url-test.cc)
target_link_libraries(download-url-test PRIVATE argus::lib::http-download doctest::doctest)
target_compile_options(download-url-test PRIVATE -Wall -Wextra)

add_test(NAME download-url-test COMMAND download-url-test)

add_executable(file-download-test
    tests/unit/download/file-download-test.cc
    tests/unit/download/fake-transport.cc
    tests/unit/download/range-server.cc)
target_link_libraries(file-download-test PRIVATE argus::lib::http-download doctest::doctest)
target_compile_options(file-download-test PRIVATE -Wall -Wextra)

add_test(NAME file-download-test COMMAND file-download-test)
