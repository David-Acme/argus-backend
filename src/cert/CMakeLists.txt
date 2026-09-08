# argus_cert: TLS certificate issuance for the instance's HTTPS listener.
# Consumers link argus::cert.

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

argus_module(NAME cert
    SOURCES
        ../shared/services/cert/cert-service.cc
    INCLUDES ..)

target_link_libraries(argus_cert
    PUBLIC
        argus_common
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_cert PRIVATE -Wall -Wextra)
