# argus_auth: the auth filter package (device fingerprint, JWT, role, JSON
# body validation) plus the JWT service it shares with the identity domain.
# Drogon controllers resolve filters by NAME at runtime (METHOD_ADD strings),
# so consumers only need this linked into the final binary, not referenced by
# symbol; identity's AuthService is the one caller that reaches these sources
# by symbol (JwtService, DeviceFilter's static helpers).
#
# A package, not a service: no database, no listener, no process. It
# authenticates by asking the identity service over argus.identity.v1, which
# is why it depends on argus-contracts and never on argus-identity.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# The leaves this module links: a DAG, so their guards carry them into every
# build, standalone or consumed.
if(NOT TARGET contract::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/auth-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/auth-contract)
endif()

if(NOT TARGET contract::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/sync-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/sync-contract)
endif()

if(NOT TARGET argus::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()
if(NOT TARGET argus::runtime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime
                     ${CMAKE_BINARY_DIR}/runtime)
endif()
if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response
                     ${CMAKE_BINARY_DIR}/response)
endif()

# The identity RPC contract (argus.identity.v1) the filter chain validates
# tokens and device credentials through.
if(NOT TARGET argus::client-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

argus_module(NAME auth
    SOURCES
        src/filter/device/device-filter.cc
        src/filter/identity-access.cc
        src/filter/jwt/jwt-filter.cc
        src/filter/role/role-filter.cc
        src/filter/valid-json/valid-json-filter.cc
        src/shared/services/jwt/jwt-service.cc
        src/shared/services/user-directory/user-directory-identity.cc
        src/shared/access/role-access.hxx
        src/shared/contracts/user-directory.hxx
        src/shared/contracts/identity-change-sink.hxx
    INCLUDES
        src
    DEPENDS
        contract::auth contract::sync argus::config argus::runtime pkg::response
        argus::client-identity
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_auth PRIVATE -Wall -Wextra)

# The role → table → permission table moved here with the vocabulary it reads
# (UserRole from contract::auth, TableName from contract::sync), so its suite
# moves with it.
enable_testing()
find_package(doctest REQUIRED)

add_executable(role-access-test tests/unit/role-access-test.cc)
target_link_libraries(role-access-test PRIVATE argus::auth doctest::doctest)
target_compile_options(role-access-test PRIVATE -Wall -Wextra)
add_test(NAME role-access-test COMMAND role-access-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
