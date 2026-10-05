argus_module(NAME camera-tapo
    SOURCES
        legacy-stok-transport.cc
        secure-passthrough-transport.cc
        tapo-api.cc
        tapo-audio.cc
        tapo-client.cc
        tapo-crypto.cc
        tapo-http.cc
        tapo-motor.cc
        tapo-talk-client.cc
        tapo-trust.cc
        tapo-ts-muxer.cc
        tapo-video.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::lib::audio
        argus::lib::runtime
        argus::lib::text
        Drogon::Drogon
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json)

target_compile_options(argus_camera-tapo PRIVATE -Wall -Wextra)
