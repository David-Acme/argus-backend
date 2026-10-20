argus_module(NAME encounter-closed
    SOURCES
        services/encounter-closed-consumer.cc
        services/maintenance-ended-consumer.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::text
        argus::lib::nats
        argus::lib::runtime
        argus::memory)

target_compile_options(argus_encounter-closed PRIVATE -Wall -Wextra)
