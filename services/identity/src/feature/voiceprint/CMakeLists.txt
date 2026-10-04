argus_module(NAME identity-voiceprint
    SOURCES
        controllers/voiceprint-controller.cc
        dtos/response-voiceprint-directory-dto.cc
        repositories/voice-device/voice-device-repository.cc
        repositories/voice-profile/voice-profile-repository.cc
        repositories/voice-sample/voice-sample-repository.cc
        schemas/voice-device/voice-device-schema.cc
        schemas/voice-profile/voice-profile-schema.cc
        schemas/voice-sample/voice-sample-schema.cc
        services/audio/speech-quality.cc
        services/audio/voice-audio.cc
        services/embedding/speaker-embedding-service.cc
        services/embedding/voice-vector.cc
        services/index/voiceprint-index.cc
        services/passive/passive-enrollment-service.cc
        services/passive/passive-policy.cc
        services/passive/voice-call-tracker.cc
        services/voiceprint/voiceprint-audit.cc
        services/voiceprint/voiceprint-feature-service.cc
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
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        Drogon::Drogon
        onnxruntime::onnxruntime
        sherpa-onnx-c-api)

target_include_directories(argus_identity-voiceprint SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/sherpa-onnx)

target_compile_options(argus_identity-voiceprint PRIVATE -Wall -Wextra)
