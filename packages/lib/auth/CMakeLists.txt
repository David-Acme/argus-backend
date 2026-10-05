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
        src/auth/role-filter.cc
        src/auth/remote-config.cc
        src/auth/remote-gate.cc
        src/auth/valid-json-filter.cc
        src/auth/jwt-service.cc
        src/auth/user-directory-identity.cc
        src/auth/role-access.hxx
        src/auth/admitted-call.hxx
        src/auth/linked-filter.hxx
        src/auth/remote-config.hxx
        src/auth/remote-gate.hxx
        src/auth/user-directory.hxx
    INCLUDES
        src
    DEPENDS
        argus::contracts::auth argus::contracts::sync argus::lib::config argus::lib::runtime argus::lib::errors argus::lib::http
        argus::clients::identity
        argus::clients::auth
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
