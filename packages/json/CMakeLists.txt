# json: JSON helpers (parse/serialize plus flat object diffs) over jsoncpp.
# Consumers link argus::json.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

argus_module(NAME json
    SOURCES
        src/shared/utils/json-util/json-util.hxx
        src/shared/utils/json-diff/json-diff.hxx
        src/shared/utils/json-diff/json-diff.cc
    INCLUDES src
    DEPENDS Drogon::Drogon)

target_compile_options(argus_json PRIVATE -Wall -Wextra)
