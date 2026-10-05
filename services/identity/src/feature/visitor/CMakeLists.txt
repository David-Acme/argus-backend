argus_module(NAME identity-visitor
    SOURCES
        controllers/visitor-controller.cc
        dtos/id-list.cc
        dtos/list-visitors-dto.cc
        dtos/merge-visitors-dto.cc
        dtos/response-visitor-crop-dto.cc
        dtos/response-visitor-detail-dto.cc
        dtos/response-visitor-dto.cc
        dtos/response-visitor-settings-dto.cc
        dtos/split-visitor-dto.cc
        dtos/update-visitor-dto.cc
        dtos/update-visitor-settings-dto.cc
        repositories/crop-capability/crop-capability-repository.cc
        repositories/sighting/sighting-repository.cc
        repositories/visitor/visitor-repository.cc
        services/visit-pattern.cc
        services/visitor-feature-service.cc
        services/visitor-policy.cc
        services/visitor-recognition-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::validation
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        Drogon::Drogon
        OpenSSL::Crypto)

target_compile_options(argus_identity-visitor PRIVATE -Wall -Wextra)
