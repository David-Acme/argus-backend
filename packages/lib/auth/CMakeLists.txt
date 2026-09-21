# argus_lib_auth: the auth filter package (device fingerprint, JWT, role, JSON
# body validation) plus the JWT service it shares with the identity domain.
# Drogon controllers resolve filters by NAME at runtime (METHOD_ADD strings),
# so consumers only need this linked into the final binary, not referenced by
# symbol; identity's AuthService is the one caller that reaches these sources
# by symbol (JwtService, DeviceFilter's static helpers).
#
# A package, not a service: no database, no listener, no process. It
# authenticates by asking the identity service over argus.identity.v1, which
# is why it depends on the identity wire — the contract and
# argus::clients::identity — and never on the identity service's sources.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

# The leaves this module links: a DAG, so their guards carry them into every
# build, standalone or consumed.
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

# The identity RPC contract (argus.identity.v1) the filter chain validates
# tokens and device credentials through.
if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

argus_lib(NAME auth
    SOURCES
        src/auth/device-filter.cc
        src/auth/details/identity-access.cc
        src/auth/jwt-filter.cc
        src/auth/role-filter.cc
        src/auth/valid-json-filter.cc
        src/auth/jwt-service.cc
        src/auth/user-directory-identity.cc
        src/auth/role-access.hxx
        src/auth/user-directory.hxx
        src/auth/identity-change-sink.hxx
    INCLUDES
        src
    DEPENDS
        argus::contracts::auth argus::contracts::sync argus::lib::config argus::lib::runtime argus::lib::errors
        argus::clients::identity
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_lib_auth PRIVATE -Wall -Wextra)

# The role → table → permission table moved here with the vocabulary it reads
# (UserRole from argus::contracts::auth, TableName from argus::contracts::sync), so its suite
# moves with it.
enable_testing()
find_package(doctest REQUIRED)

add_executable(role-access-test tests/unit/role-access-test.cc)
target_link_libraries(role-access-test PRIVATE argus::lib::auth doctest::doctest)
target_compile_options(role-access-test PRIVATE -Wall -Wextra)
add_test(NAME role-access-test COMMAND role-access-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
