# argus_mdns: mDNS announcement of the instance's endpoint on the LAN.
# Consumers link argus::mdns.

find_package(Drogon CONFIG REQUIRED)
find_package(mdns REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_module(NAME mdns
    SOURCES
        ../shared/services/mdns/mdns-service.cc
    INCLUDES ..)

target_link_libraries(argus_mdns
    PUBLIC
        argus_common
        Drogon::Drogon
        mdns::mdns
        nlohmann_json::nlohmann_json)

# Third-party mdns.h is header-only: treat its include dir as SYSTEM so
# -Wall -Wextra never reports warnings from the vendored header.
if(TARGET mdns::mdns)
    get_target_property(_argus_mdns_dirs mdns::mdns
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(_argus_mdns_dirs)
        set_target_properties(mdns::mdns PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_argus_mdns_dirs}")
    endif()
endif()

target_compile_options(argus_mdns PRIVATE -Wall -Wextra)
