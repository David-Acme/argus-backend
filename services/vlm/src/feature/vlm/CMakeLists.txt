argus_module(NAME vlm
    SOURCES
        controllers/vlm-controller.cc
        dtos/describe-dto.cc
        services/remote/remote-vision-adapter.cc
        services/remote/vlm-remote.cc
        services/vision-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::config
        argus::hardware-profile
        argus::lib::runtime
        argus::lib::validation
        argus::lib::http
        Drogon::Drogon
        opencv::opencv
        nlohmann_json::nlohmann_json
        llama
        mtmd)

target_include_directories(argus_vlm SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party)

target_compile_options(argus_vlm PRIVATE -Wall -Wextra)
