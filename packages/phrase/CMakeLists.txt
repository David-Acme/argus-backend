# argus_phrase: phrase matching and the rule syntax built on it — the
# automaton, the catalog that wraps it and the rule parser that reads the
# catalog. argus-memory (formation and recall) and argus-voice (reaction
# rules) both use the cluster, so it is a module rather than either
# service's source.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_module(NAME phrase
    SOURCES
        src/shared/services/memory/phrase-catalog.cc
        src/shared/services/memory/rule-parser.cc
        src/shared/utils/text-match/phrase-automaton.cc
    INCLUDES
        src
    DEPENDS
        argus_common)

target_compile_options(argus_phrase PRIVATE -Wall -Wextra)
