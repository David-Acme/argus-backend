argus_module(NAME llm
    SOURCES
        controllers/llm-controller.cc
        dtos/chat-dto.cc
        services/intent-gate.cc
        services/lfm-adapter.cc
        services/llm-service.cc
        services/sampling-config.cc
        services/tools/app-command.cc
        services/tools/core-tools.cc
        services/tools/module-command-lexicon.cc
        services/tools/module-command.cc
        services/tools/module-offer.cc
        services/tools/remote-tool.cc
        services/tools/reply-claim-lexicon.cc
        services/tools/reply-claims.cc
        services/tools/spoken-intent.cc
        services/tools/time-arguments.cc
        services/tools/tool-access.cc
        services/tools/tool-directory.cc
        services/tools/tool-executor.cc
        services/tools/tool-grounding.cc
        services/tools/tool-registry.cc
        services/turn/deciders.cc
        services/turn/decision-tally.cc
        services/turn/model-text.cc
        services/turn/pending-turn.cc
        services/turn/slot-lexicon.cc
        services/turn/slots.cc
        services/turn/turn-flow.cc
        services/turn/turn-texts.cc
    INCLUDES
        ../..
    DEPENDS
        argus::llm-shared
        argus::lib::config
        argus::lib::auth
        argus::lib::mcp
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
