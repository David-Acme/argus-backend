argus_module(NAME guard-presence-repository
    SOURCES
        presence-repository.cc
    DEPENDS
        argus::lib::sqlite
        Drogon::Drogon
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_compile_options(argus_guard-presence-repository PRIVATE -Wall -Wextra)
