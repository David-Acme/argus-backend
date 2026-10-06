argus_module(NAME vlm-components
    SOURCES
        infra/vision-fetch.cc
        services/vision-component-host.cc
    INCLUDES
        ../..
    DEPENDS
        argus::contracts::settings-wire
        argus::lib::http-download
        argus::lib::runtime)

target_compile_options(argus_vlm-components PRIVATE -Wall -Wextra)
