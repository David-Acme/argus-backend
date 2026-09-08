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

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

# The identity RPC contract (argus.identity.v1) the filter chain validates
# tokens and device credentials through.
if(NOT TARGET argus::sdk-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-contracts
                     ${CMAKE_BINARY_DIR}/argus-contracts EXCLUDE_FROM_ALL)
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
    INCLUDES
        src
    DEPENDS
        argus_common
        argus::sdk-identity
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_auth PRIVATE -Wall -Wextra)
