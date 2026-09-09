# argus_cert: TLS certificate issuance for the instance's HTTPS listener.
# Consumers link argus::cert.

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

argus_module(NAME cert
    SOURCES
        src/shared/services/cert/cert-service.cc
    INCLUDES src)

target_link_libraries(argus_cert
    PUBLIC
        argus_common
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_cert PRIVATE -Wall -Wextra)

# CertService rotation and the in-process TLS listener that proves the hot
# reload. It rides the identity domain, which is why it links argus_identity.
enable_testing()
find_package(doctest REQUIRED)

add_executable(cert-san-test tests/unit/cert-san-test.cc)
target_link_libraries(cert-san-test PRIVATE argus::cert argus_identity
    argus_common doctest::doctest)
target_compile_options(cert-san-test PRIVATE -Wall -Wextra)
add_test(NAME cert-san-test COMMAND cert-san-test)
