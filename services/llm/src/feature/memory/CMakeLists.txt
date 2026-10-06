argus_module(NAME memory
    SOURCES
        infra/catalog-replica.cc
        infra/notification-reminder-calls.cc
        infra/productivity-reminder-rows.cc
        repositories/memory-graph/memory-graph-repository.cc
        services/embedding/embedding-service.cc
        services/embedding/unigram-tokenizer.cc
        services/extract/call-time.cc
        services/extract/extraction-service.cc
        services/extract/lexicon-extractor.cc
        services/extract/temporal-resolver.cc
        services/extract/tiered-extractor.cc
        services/memory/entity-resolver.cc
        services/memory/graph-recall.cc
        services/memory/memory-formation.cc
        services/memory/memory-service.cc
        services/memory/memory-tool-descriptors.cc
        services/memory/sqlite-graph.cc
        services/memory/tool-parser.cc
    INCLUDES
        ../..
    DEPENDS
        argus::llm-shared
        argus::lib::config
        argus::lib::phrase
        argus::lib::sqlite
        argus::lib::text
        argus::lib::runtime
        argus::lib::nats
        argus::contracts::sync
        argus::clients::llm
        argus::clients::notification
        argus::clients::productivity
        argus::hardware-profile
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        llama
        onnxruntime::onnxruntime
        sqlite-vec)

target_include_directories(argus_memory SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party)

target_compile_options(argus_memory PRIVATE -Wall -Wextra)
