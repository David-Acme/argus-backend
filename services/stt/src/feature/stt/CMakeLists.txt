argus_module(NAME stt
    SOURCES
        controllers/stt-controller.cc
        dtos/transcribe-dto.cc
        services/stt-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::config
        argus::lib::runtime
        argus::lib::http
        argus::lib::validation
        argus::clients::stt
        argus::contracts::stt
        Drogon::Drogon
        onnxruntime::onnxruntime
        nlohmann_json::nlohmann_json
        sherpa-onnx-c-api)

target_include_directories(argus_stt SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/sherpa-onnx)

target_compile_options(argus_stt PRIVATE -Wall -Wextra)
