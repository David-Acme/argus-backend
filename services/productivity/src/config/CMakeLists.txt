argus_module(NAME productivity-config
    SOURCES
        productivity-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http)

target_compile_options(argus_productivity-config PRIVATE -Wall -Wextra)
