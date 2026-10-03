argus_module(NAME tts-speech-text
    SOURCES
        text/number-words.cc
        text/text-normalizer.cc
        text/prosodic-chunker.cc
    INCLUDES ../..)

target_compile_options(argus_tts-speech-text PRIVATE -Wall -Wextra)

argus_module(NAME tts-synthesis
    SOURCES
        services/tts-service.cc
        infra/supertonic/tts-engine.cc
        infra/supertonic/onnx-utils.cc
        infra/supertonic/style.cc
        infra/supertonic/unicode-processor.cc
        infra/pocket/pocket-bundle.cc
        infra/pocket/unigram-tokenizer.cc
        infra/pocket/pocket-voice.cc
        infra/pocket/pocket-prompt.cc
        infra/pocket/pocket-engine.cc
        infra/pocket/pcm-rate-converter.cc
        infra/pocket/reference-audio.cc
    INCLUDES ../..
    DEPENDS
        argus::tts-speech-text
        argus::lib::audio
        argus::lib::config argus::lib::runtime
        argus::clients::tts
        argus::hardware-profile
        Drogon::Drogon
        onnxruntime::onnxruntime
        nlohmann_json::nlohmann_json)

target_compile_options(argus_tts-synthesis PRIVATE -Wall -Wextra)

argus_module(NAME tts-synthesis-http
    SOURCES
        controllers/tts-controller.cc
        dtos/synthesize-dto.cc
    INCLUDES ../..
    DEPENDS
        argus::tts-synthesis
        argus::lib::auth
        argus::lib::http
        argus::lib::validation
        argus::contracts::tts)

target_compile_options(argus_tts-synthesis-http PRIVATE -Wall -Wextra)
