cmake_minimum_required(VERSION 3.25)

# mdns: the announcement the gateway makes so the appliance can be found on the
# LAN. One implementation, one consumer, and the plan's D-table moves it here
# from services/gateway, where it was a source of gateway-core; consumers link
# argus::lib::mdns.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

# The service reads mdns.* through ConfigService, so the edge is argus::lib::config;
# its public header carries a Json::Value, so Drogon::Drogon is declared too.
if(NOT TARGET argus::lib::config)
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

argus_lib(NAME mdns
    SOURCES
        src/mdns/mdns-service.cc
        src/mdns/mdns-service.hxx
    INCLUDES src
    DEPENDS
        argus::lib::config
        Drogon::Drogon)

# The library is an implementation detail: the public header carries jsoncpp
# and nothing else, so nothing that links argus::lib::mdns inherits mdns.h.
target_link_libraries(argus_lib_mdns PRIVATE mdns::mdns)

target_compile_options(argus_lib_mdns PRIVATE -Wall -Wextra)

# The announcement's own suite: the disabled path a boot must survive, and the
# mdns.* keys as the health report shows them. Nothing here opens a socket or
# announces on the LAN -- every case runs with advertising off.
enable_testing()
find_package(doctest REQUIRED)

add_executable(mdns-service-test tests/unit/mdns-service-test.cc)
target_link_libraries(mdns-service-test PRIVATE
    argus::lib::mdns
    doctest::doctest)
target_compile_options(mdns-service-test PRIVATE -Wall -Wextra)
add_test(NAME mdns-service-test COMMAND mdns-service-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
