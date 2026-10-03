argus_module(NAME identity-voiceprint
    SOURCES
        controllers/voiceprint-controller.cc
        dtos/create-voiceprint-challenge-dto.cc
        dtos/enroll-voiceprint-dto.cc
        dtos/response-voiceprint-challenge-dto.cc
        dtos/response-voiceprint-sample-dto.cc
        dtos/response-voiceprint-status-dto.cc
        dtos/response-voiceprint-verify-dto.cc
        dtos/voice-sample-dto.cc
        repositories/voiceprint/voiceprint-repository.cc
        repositories/voiceprint-challenge/voiceprint-challenge-repository.cc
        schemas/voiceprint/voiceprint-schema.cc
        schemas/voiceprint-challenge/voiceprint-challenge-schema.cc
        services/audio/speech-quality.cc
        services/audio/voice-audio.cc
        services/embedding/speaker-embedding-service.cc
        services/embedding/voice-vector.cc
        services/index/voiceprint-index.cc
        services/voiceprint/voiceprint-feature-service.cc
        services/voiceprint/voiceprint-phrases.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-config
        argus::identity-shared
        argus::lib::audio
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::lib::validation
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        argus::contracts::voice
        Drogon::Drogon
        OpenSSL::Crypto
        onnxruntime::onnxruntime
        sherpa-onnx-c-api)

target_include_directories(argus_identity-voiceprint SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/sherpa-onnx)

target_compile_options(argus_identity-voiceprint PRIVATE -Wall -Wextra)
