argus_module(NAME vlm
    SOURCES
        controllers/vlm-controller.cc
        dtos/describe-dto.cc
        services/jpeg-gate.cc
        services/vision-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::contracts::vlm
        argus::lib::config
        argus::hardware-profile
        argus::lib::runtime
        argus::lib::validation
        argus::lib::http
        Drogon::Drogon
        opencv::opencv
        llama
        mtmd)

target_include_directories(argus_vlm SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party)

target_compile_options(argus_vlm PRIVATE -Wall -Wextra)
