# argus_clients_vlm: the argus-vlm internal wire seen from the caller's side.
# argus-guard is the only consumer; the client owns the envelope and the
# base64 request encoding so no service hand-rolls the wire (rule 23).

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/text
                     ${CMAKE_CURRENT_BINARY_DIR}/text)
endif()

find_package(Drogon CONFIG REQUIRED)

argus_clients(NAME vlm
    SOURCES
        src/shared/services/vision/remote/vlm-client.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_clients_vlm PRIVATE -Wall -Wextra)
