argus_module(NAME llm-shared
    HEADER_ONLY
    SOURCES
        vocabulary/tool-contracts.hxx
    INCLUDES
        ..
    DEPENDS
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::mcp)
