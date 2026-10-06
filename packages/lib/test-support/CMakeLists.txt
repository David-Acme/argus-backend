include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_lib(NAME test-support
    HEADER_ONLY
    SOURCES
        src/test-support/app-runner.hxx
    INCLUDES src
    DEPENDS Drogon::Drogon)
