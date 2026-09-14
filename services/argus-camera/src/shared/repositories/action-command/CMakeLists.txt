argus_module(NAME camera-action-command
    SOURCES
        action-command-repository.cc
    DEPENDS
        argus_common
        argus::sqlite
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
