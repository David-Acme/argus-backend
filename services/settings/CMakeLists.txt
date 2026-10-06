cmake_minimum_required(VERSION 3.25)
project(argus-settings VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_contracts_substrate()

set(SETTINGS_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(cnats CONFIG REQUIRED)

    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::lib::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::lib::text)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/text
                         ${CMAKE_BINARY_DIR}/text)
    endif()
    if(NOT TARGET argus::lib::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
endif()

if(NOT TARGET argus::clients::settings)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/settings
                     ${CMAKE_BINARY_DIR}/clients/settings)
endif()

find_package(doctest REQUIRED)

add_subdirectory(${SETTINGS_SRC_ROOT}/config)
add_subdirectory(${SETTINGS_SRC_ROOT}/app/rpc)

file(GLOB SETTINGS_FEATURE_MODULES CONFIGURE_DEPENDS
    "${SETTINGS_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS SETTINGS_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

argus_service(NAME argus-settings
    MAIN ${SETTINGS_SRC_ROOT}/app/main.cc
    MODULES
        argus::settings-gateway
        argus::settings-modules
        argus::settings-rpc
        argus::settings-config
        argus::clients::settings
        argus::lib::auth
        argus::lib::config
        argus::lib::http
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
    PORTS 7045 7047)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

foreach(TEST_NAME IN ITEMS settings-gateway-test settings-dto-test settings-config-test settings-profile-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
    target_link_libraries(${TEST_NAME} PRIVATE
        argus::settings-gateway
        argus::contracts::settings-wire
        doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

target_compile_definitions(settings-profile-test PRIVATE
    ARGUS_SETTINGS_PROFILES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/profiles.json")

foreach(TEST_NAME IN ITEMS settings-modules-test settings-module-engine-test settings-modules-http-test
                         settings-module-journal-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
    target_link_libraries(${TEST_NAME} PRIVATE
        argus::settings-modules
        argus::settings-rpc
        argus::contracts::settings-wire
        doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    target_compile_definitions(${TEST_NAME} PRIVATE
        ARGUS_SETTINGS_MODULES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/modules.json"
        ARGUS_SETTINGS_SCHEMA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
