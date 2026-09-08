# argus_identity: identity domain (auth, user, invitation, pairing) plus the
# repositories, schemas and services its routes need to link. The legacy
# backend and the argus-gateway both consume this one definition. Links
# argus_auth PUBLIC because AuthService calls JwtService and DeviceFilter's
# static helpers directly.

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenCV CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

set(ARGUS_IDENTITY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/controllers/auth-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/create-device-login-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/device-login-status-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/login-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/refresh-token-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/register-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/response-login-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/response-refresh-token-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/dtos/update-me-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/auth/services/auth-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/invitation/controllers/invitation-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/invitation/dtos/create-invitation-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/invitation/dtos/resolve-invitation-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/invitation/dtos/response-invitation-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/invitation/dtos/response-invitation-resolve-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/invitation/services/invitation-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/pairing/controllers/pairing-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/pairing/dtos/pairing-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/pairing/dtos/response-pairing-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/user/controllers/portrait-preview-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/user/controllers/user-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/user/dtos/response-portrait-preview-capability-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/user/dtos/response-portrait-preview-image-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/user/dtos/update-user-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/user/services/portrait-preview-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/api/user/services/user-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/device-credential/device-credential-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/device-login-challenge/device-login-challenge-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/face-embedding/face-embedding-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/person/person-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/portrait-preview-capability/portrait-preview-capability-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/refresh-token/refresh-token-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/stored-file/stored-file-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/user-invitation/user-invitation-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/user-portrait/user-portrait-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/user/user-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/device-credential/device-credential-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/device-login-challenge/device-login-challenge-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/face-embedding/face-embedding-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/person/person-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/portrait-preview-capability/portrait-preview-capability-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/refresh-token/refresh-token-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/stored-file/stored-file-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/user-invitation/user-invitation-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/user-portrait/user-portrait-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/user/user-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/services/face/face-db.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/services/face/face-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/services/storage/private-portrait-service.cc
)

add_library(argus_identity STATIC ${ARGUS_IDENTITY_SOURCES})

target_include_directories(argus_identity
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_identity PUBLIC ${ARGUS_NCNN_TARGET})
    target_include_directories(argus_identity SYSTEM PUBLIC
        ${CMAKE_BINARY_DIR}/third_party/ncnn/src)
endif()

target_link_libraries(argus_identity
    PUBLIC
        argus_common
        argus::auth
        argus::audit
        argus::sqlite
        argus::cert
        argus::socket
        argus::hardware-profile-gpu
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        opencv::opencv
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_identity PRIVATE -Wall -Wextra)
