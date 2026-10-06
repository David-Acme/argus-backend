include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()
if(NOT TARGET argus::lib::runtime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime
                     ${CMAKE_BINARY_DIR}/runtime)
endif()
if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../errors
                     ${CMAKE_BINARY_DIR}/errors)
endif()
if(NOT TARGET argus::lib::http)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../http
                     ${CMAKE_BINARY_DIR}/http)
endif()

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::lib::nats)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../nats
                     ${CMAKE_BINARY_DIR}/nats)
endif()
if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../text
                     ${CMAKE_BINARY_DIR}/text)
endif()

if(NOT TARGET argus::lib::mcp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../mcp
                     ${CMAKE_BINARY_DIR}/mcp)
endif()

if(NOT TARGET argus::clients::settings)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../clients/settings
                     ${CMAKE_BINARY_DIR}/clients/settings)
endif()

if(NOT TARGET argus::clients::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../clients/auth
                     ${CMAKE_BINARY_DIR}/clients/auth EXCLUDE_FROM_ALL)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

argus_lib(NAME auth
    SOURCES
        src/auth/auth-access.cc
        src/auth/device-filter.cc
        src/auth/details/identity-access.cc
        src/auth/details/proxy-allowlist.cc
        src/auth/jwt-filter.cc
        src/auth/module-feed.cc
        src/auth/module-gate.cc
        src/auth/module-settings-read.cc
        src/auth/role-filter.cc
        src/auth/tool-gate.cc
        src/auth/remote-config.cc
        src/auth/remote-gate.cc
        src/auth/valid-json-filter.cc
        src/auth/jwt-service.cc
        src/auth/user-directory-identity.cc
        src/auth/tool-gate.hxx
        src/auth/role-access.hxx
        src/auth/capability.hxx
        src/auth/module-snapshot.hxx
        src/auth/admitted-call.hxx
        src/auth/linked-filter.hxx
        src/auth/module-feed.hxx
        src/auth/module-gate.hxx
        src/auth/remote-config.hxx
        src/auth/remote-gate.hxx
        src/auth/user-directory.hxx
    INCLUDES
        src
    DEPENDS
        argus::contracts::auth argus::contracts::sync argus::lib::config argus::lib::runtime argus::lib::errors argus::lib::http
        argus::clients::identity
        argus::clients::auth
        argus::clients::settings
        argus::lib::mcp
        argus::lib::nats
        argus::lib::text
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_lib_auth PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(role-access-test tests/unit/role-access-test.cc)
target_link_libraries(role-access-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(role-access-test PRIVATE -Wall -Wextra)
add_test(NAME role-access-test COMMAND role-access-test)

add_executable(capability-test tests/unit/capability-test.cc)
target_link_libraries(capability-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(capability-test PRIVATE -Wall -Wextra)
add_test(NAME capability-test COMMAND capability-test)

add_executable(tool-gate-test tests/unit/tool-gate-test.cc)
target_link_libraries(tool-gate-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(tool-gate-test PRIVATE -Wall -Wextra)
add_test(NAME tool-gate-test COMMAND tool-gate-test)

add_executable(module-gate-test tests/unit/module-gate-test.cc)
target_link_libraries(module-gate-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(module-gate-test PRIVATE -Wall -Wextra)
add_test(NAME module-gate-test COMMAND module-gate-test)

add_executable(remote-gate-test tests/unit/remote-gate-test.cc)
target_link_libraries(remote-gate-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(remote-gate-test PRIVATE -Wall -Wextra)
add_test(NAME remote-gate-test COMMAND remote-gate-test)

add_executable(proxy-allowlist-test tests/unit/proxy-allowlist-test.cc)
target_link_libraries(proxy-allowlist-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(proxy-allowlist-test PRIVATE -Wall -Wextra)
add_test(NAME proxy-allowlist-test COMMAND proxy-allowlist-test)

add_executable(device-origin-test tests/unit/device-origin-test.cc)
target_link_libraries(device-origin-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(device-origin-test PRIVATE -Wall -Wextra)
add_test(NAME device-origin-test COMMAND device-origin-test)

add_executable(admitted-call-test tests/unit/admitted-call-test.cc)
target_link_libraries(admitted-call-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(admitted-call-test PRIVATE -Wall -Wextra)
add_test(NAME admitted-call-test COMMAND admitted-call-test)

add_executable(jwt-service-test tests/unit/jwt-service-test.cc)
target_link_libraries(jwt-service-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(jwt-service-test PRIVATE -Wall -Wextra)
add_test(NAME jwt-service-test COMMAND jwt-service-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
