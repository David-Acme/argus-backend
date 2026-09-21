cmake_minimum_required(VERSION 3.25)

# mdns: the announcement the gateway makes so the appliance can be found on the
# LAN. One implementation, one consumer, and the plan's D-table moves it here
# from services/gateway, where it was a source of gateway-core; consumers link
# argus::mdns.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

# The service reads mdns.* through ConfigService, so the edge is argus::config;
# its public header carries a Json::Value, so Drogon::Drogon is declared too.
if(NOT TARGET argus::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()

find_package(mdns REQUIRED)

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

argus_module(NAME mdns
    SOURCES
        src/mdns/mdns-service.cc
        src/mdns/mdns-service.hxx
    INCLUDES src
    DEPENDS
        argus::config
        Drogon::Drogon)

# The library is an implementation detail: the public header carries jsoncpp
# and nothing else, so nothing that links argus::mdns inherits mdns.h.
target_link_libraries(argus_mdns PRIVATE mdns::mdns)

target_compile_options(argus_mdns PRIVATE -Wall -Wextra)
