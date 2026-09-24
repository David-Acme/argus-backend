argus_module(NAME llm
    SOURCES
        controllers/llm-controller.cc
        dtos/chat-dto.cc
        services/intent-gate.cc
        services/lfm-adapter.cc
        services/llm-service.cc
        services/tools/tool-executor.cc
        services/tools/tool-registry.cc
        services/tools/tool-validator.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::config
        argus::lib::auth
        argus::hardware-profile
        argus::lib::phrase
        argus::lib::runtime
        argus::lib::validation
        argus::lib::http
        argus::clients::llm
        argus::intent
        memory-core
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        argus::contracts::auth
        llama)

target_include_directories(argus_llm SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party)

target_compile_options(argus_llm PRIVATE -Wall -Wextra)
