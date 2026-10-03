argus_module(NAME llm
    SOURCES
        controllers/llm-controller.cc
        dtos/chat-dto.cc
        services/intent-gate.cc
        services/lfm-adapter.cc
        services/llm-service.cc
        services/sampling-config.cc
        services/tools/app-command.cc
        services/tools/app-tool-descriptors.cc
        services/tools/tool-executor.cc
        services/tools/tool-registry.cc
        services/tools/tool-validator.cc
    INCLUDES
        ../..
    DEPENDS
        argus::llm-shared
        argus::lib::config
        argus::lib::auth
        argus::hardware-profile
        argus::lib::phrase
        argus::lib::text
        argus::lib::runtime
        argus::lib::validation
        argus::lib::http
        argus::clients::llm
        argus::intent
        argus::memory
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        argus::contracts::auth
        llama)

target_include_directories(argus_llm SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party)

target_compile_options(argus_llm PRIVATE -Wall -Wextra)
