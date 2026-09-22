argus_module(NAME productivity-change-outbox
    SOURCES
        change-outbox-repository.cc
    DEPENDS
        argus::lib::sqlite
        argus::lib::text
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
