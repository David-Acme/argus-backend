# argus_identity: the identity service (auth/invitation/pairing/user
# features, the identity repositories and schemas, faces and portraits).
# Compiled INTO the argus-gateway binary for now — no new process, no new
# port; the later standalone extraction is a CMake/deploy change, not a
# second file move. The root project and every standalone consumer build
# share this one definition.

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenCV CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

argus_module(NAME identity
    SOURCES
        src/feature/api/auth/controllers/auth-controller.cc
        src/feature/api/auth/dtos/create-device-login-dto.cc
        src/feature/api/auth/dtos/device-login-status-dto.cc
        src/feature/api/auth/dtos/login-dto.cc
        src/feature/api/auth/dtos/refresh-token-dto.cc
        src/feature/api/auth/dtos/register-dto.cc
        src/feature/api/auth/dtos/response-login-dto.cc
        src/feature/api/auth/dtos/response-refresh-token-dto.cc
        src/feature/api/auth/dtos/update-me-dto.cc
        src/feature/api/auth/services/auth-service.cc
        src/feature/api/invitation/controllers/invitation-controller.cc
        src/feature/api/invitation/dtos/create-invitation-dto.cc
        src/feature/api/invitation/dtos/resolve-invitation-dto.cc
        src/feature/api/invitation/dtos/response-invitation-dto.cc
        src/feature/api/invitation/dtos/response-invitation-resolve-dto.cc
        src/feature/api/invitation/services/invitation-feature-service.cc
        src/feature/api/pairing/controllers/pairing-controller.cc
        src/feature/api/pairing/dtos/pairing-dto.cc
        src/feature/api/pairing/dtos/response-pairing-dto.cc
        src/feature/api/user/controllers/portrait-preview-controller.cc
        src/feature/api/user/controllers/user-controller.cc
        src/feature/api/user/dtos/response-portrait-preview-capability-dto.cc
        src/feature/api/user/dtos/response-portrait-preview-image-dto.cc
        src/feature/api/user/dtos/update-user-dto.cc
        src/feature/api/user/services/portrait-preview-service.cc
        src/feature/api/user/services/user-feature-service.cc
        src/shared/repositories/device-credential/device-credential-repository.cc
        src/shared/repositories/device-login-challenge/device-login-challenge-repository.cc
        src/shared/repositories/face-embedding/face-embedding-repository.cc
        src/shared/repositories/person/person-repository.cc
        src/shared/repositories/portrait-preview-capability/portrait-preview-capability-repository.cc
        src/shared/repositories/refresh-token/refresh-token-repository.cc
        src/shared/repositories/stored-file/stored-file-repository.cc
        src/shared/repositories/user-invitation/user-invitation-repository.cc
        src/shared/repositories/user-portrait/user-portrait-repository.cc
        src/shared/repositories/user/user-repository.cc
        src/shared/schemas/device-credential/device-credential-schema.cc
        src/shared/schemas/device-login-challenge/device-login-challenge-schema.cc
        src/shared/schemas/face-embedding/face-embedding-schema.cc
        src/shared/schemas/person/person-schema.cc
        src/shared/schemas/portrait-preview-capability/portrait-preview-capability-schema.cc
        src/shared/schemas/refresh-token/refresh-token-schema.cc
        src/shared/schemas/stored-file/stored-file-schema.cc
        src/shared/schemas/user-invitation/user-invitation-schema.cc
        src/shared/schemas/user-portrait/user-portrait-schema.cc
        src/shared/schemas/user/user-schema.cc
        src/shared/services/face/face-db.cc
        src/shared/services/face/face-service.cc
        src/shared/services/storage/private-portrait-service.cc
    INCLUDES
        src
        ../third_party
        ../third_party/stb
    DEPENDS
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

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_identity PUBLIC ${ARGUS_NCNN_TARGET})
    target_include_directories(argus_identity SYSTEM PUBLIC
        ${CMAKE_BINARY_DIR}/third_party/ncnn/src)
endif()

target_compile_options(argus_identity PRIVATE -Wall -Wextra)

# The identity unit suites are root-project tests: they are part of the root
# ctest run, and the identity-migration library they ride exists only in the
# root project (tools/ is EXCLUDE_FROM_ALL there). Standalone consumer
# builds (gateway, camera, productivity, notification) never had these
# suites and still don't.
if(ARGUS_ROOT_PROJECT)
    find_package(doctest REQUIRED)

    # The migration suite: schema apply + argus.db -> identity.db verified
    # row by row, against the schema this folder owns.
    add_executable(identity-migration-test tests/unit/identity-migration-test.cc)

    target_link_libraries(identity-migration-test PRIVATE identity-migration
        doctest::doctest)

    target_compile_options(identity-migration-test PRIVATE -Wall -Wextra)

    add_test(NAME identity-migration-test COMMAND identity-migration-test)

    # The device credential suite rides the identity domain: the DeviceFilter
    # gate, the credential repository and the auth-service issuance flow.
    add_executable(device-credential-test tests/unit/device-credential-test.cc)

    target_link_libraries(device-credential-test PRIVATE argus_identity
        doctest::doctest)

    target_compile_options(device-credential-test PRIVATE -Wall -Wextra)

    add_test(NAME device-credential-test COMMAND device-credential-test)

    get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(DOCTEST_INCLUDE_DIRS)
        set_target_properties(doctest::doctest PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
    endif()
endif()
