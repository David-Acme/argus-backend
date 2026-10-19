argus_module(NAME stt-spot
    SOURCES
        services/keyword-spotter.cc
    INCLUDES
        ../..
    DEPENDS
        argus::stt-config
        argus::lib::config
        Drogon::Drogon
        sherpa-onnx-c-api)

target_include_directories(argus_stt-spot SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/sherpa-onnx)

target_compile_options(argus_stt-spot PRIVATE -Wall -Wextra)
