argus_module(NAME productivity-dtos
    SOURCES
        idempotency-key/idempotency-key-dto.cc
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
    DEPENDS
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_productivity-dtos PRIVATE -Wall -Wextra)
