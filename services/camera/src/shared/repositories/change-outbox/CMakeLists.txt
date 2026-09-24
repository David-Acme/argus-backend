argus_module(NAME camera-change-outbox
    SOURCES
        change-outbox-repository.cc
    DEPENDS
        argus::lib::sqlite
        argus::lib::text
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_compile_options(argus_camera-change-outbox PRIVATE -Wall -Wextra)
