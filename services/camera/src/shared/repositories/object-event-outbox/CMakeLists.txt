argus_module(NAME camera-object-event-outbox
    SOURCES
        object-event-outbox-repository.cc
    DEPENDS
        argus::lib::sqlite
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
