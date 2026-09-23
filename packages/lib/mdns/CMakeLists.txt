cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_BINARY_DIR}/config)
endif()

find_package(mdns REQUIRED)

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

target_link_libraries(argus_lib_mdns PRIVATE mdns::mdns)

target_compile_options(argus_lib_mdns PRIVATE -Wall -Wextra)

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
