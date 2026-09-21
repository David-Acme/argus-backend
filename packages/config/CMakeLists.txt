cmake_minimum_required(VERSION 3.25)

# config: service boot helpers — the TOML config-service reader and the IService
# interface every service boot wires. It answers nothing over HTTP and owns no
# refusal, so it depends on neither argus::http nor argus::errors; the health
# controller and the listener resolution moved to argus::http.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(tomlplusplus REQUIRED)
endif()

argus_module(NAME config
    SOURCES
        src/config/service.hxx
        src/shared/services/config-service/config-service.cc
        src/shared/services/config-service/config-service.hxx
    INCLUDES src
    DEPENDS
        Drogon::Drogon
        tomlplusplus::tomlplusplus)

foreach(_tomlplusplus_include_var
        tomlplusplus_INCLUDE_DIRS_RELEASE
        tomlplusplus_INCLUDE_DIRS_DEBUG
        tomlplusplus_INCLUDE_DIRS)
    if(DEFINED ${_tomlplusplus_include_var})
        target_include_directories(argus_config SYSTEM PUBLIC
            ${${_tomlplusplus_include_var}})
    endif()
endforeach()

target_compile_options(argus_config PRIVATE -Wall -Wextra)
