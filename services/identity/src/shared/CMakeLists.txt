argus_module(NAME identity-shared
    SOURCES
        repositories/face-embedding/face-embedding-repository.cc
        repositories/person/person-repository.cc
        repositories/person-snapshot/person-snapshot-repository.cc
        repositories/person-tag/person-tag-repository.cc
        repositories/portrait-preview-capability/portrait-preview-capability-repository.cc
        repositories/privacy/privacy-repository.cc
        repositories/stored-file/stored-file-repository.cc
        repositories/user-invitation/user-invitation-repository.cc
        repositories/user-portrait/user-portrait-repository.cc
        repositories/user/user-repository.cc
        schemas/face-embedding/face-embedding-schema.cc
        schemas/person/person-schema.cc
        schemas/portrait-preview-capability/portrait-preview-capability-schema.cc
        schemas/privacy/privacy-schema.cc
        schemas/stored-file/stored-file-schema.cc
        schemas/user-invitation/user-invitation-schema.cc
        schemas/user-portrait/user-portrait-schema.cc
        schemas/user/user-schema.cc
        services/face/face-db.cc
        services/face/face-quality.cc
        services/face/face-service.cc
        services/face-crop/face-crop-store.cc
        repositories/visitor-setting/visitor-setting-repository.cc
        services/privacy/privacy-gate.cc
        services/schema/column-migration.cc
        services/storage/private-portrait-service.cc
        services/token/opaque-token.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::storage
        argus::lib::text
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        argus::hardware-profile
        argus::hardware-profile-gpu
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        opencv::opencv
        OpenSSL::Crypto)

target_include_directories(argus_identity-shared SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/ncnn/src
    ${CMAKE_BINARY_DIR}/third_party/ncnn/src)

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_identity-shared PUBLIC ${ARGUS_NCNN_TARGET})
endif()

target_compile_options(argus_identity-shared PRIVATE -Wall -Wextra)
