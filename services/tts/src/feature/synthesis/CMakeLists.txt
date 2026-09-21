argus_module(NAME tts-synthesis
    SOURCES
        domain/tts-service.cc
        infra/supertonic/tts-engine.cc
        infra/supertonic/onnx-utils.cc
        infra/supertonic/style.cc
        infra/supertonic/unicode-processor.cc
    INCLUDES ../..
    DEPENDS
        argus::lib::config argus::lib::runtime
        argus::clients::tts
        argus::hardware-profile
        Drogon::Drogon
        onnxruntime::onnxruntime
        nlohmann_json::nlohmann_json)

target_compile_options(argus_tts-synthesis PRIVATE -Wall -Wextra)

# The HTTP face of synthesis: the controller answers through the shared
# envelope, so argus::lib::http is on the edge.
argus_module(NAME tts-synthesis-http
    SOURCES
        api/http/controller/tts-controller.cc
        api/http/dto/synthesize-dto.cc
    INCLUDES ../..
    DEPENDS
        argus::tts-synthesis
        argus::lib::auth
        argus::lib::http
        argus::lib::validation
        argus::contracts::tts)

target_compile_options(argus_tts-synthesis-http PRIVATE -Wall -Wextra)
