# argus_auth: the auth filter chain (device fingerprint, JWT, role, JSON-body
# validation) plus the JWT service it shares with the identity domain.
# Drogon controllers resolve filters by name at runtime (METHOD_ADD strings),
# so consumers only need this linked into the final binary, not referenced
# by symbol; identity's AuthService is the one caller that does reach these
# sources by symbol (JwtService, DeviceFilter's static helpers).

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

argus_module(NAME auth
    SOURCES
        device/device-filter.cc
        jwt/jwt-filter.cc
        role/role-filter.cc
        valid-json/valid-json-filter.cc
        ../shared/services/jwt/jwt-service.cc
    INCLUDES
        ..
    DEPENDS
        argus_common
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_auth PRIVATE -Wall -Wextra)
