# argus_common: shared foundation library consumed by the backend, the unit
# tests, the labs and the argus-gateway service.

set(ARGUS_COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/enums.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/access/role-access.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/validation/rules.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/validation/validation_dsl.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/validation/validator.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/exceptions/response-exception.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/api-response/api-response.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/api-response/api-response.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/thread-budget/thread-budget.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/thread-budget/thread-budget.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/blocking-task/blocking-task.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/cancellation/cancellation-token.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/hardware-profile/hardware-profile.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/hardware-profile/hardware-profile.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/nats/nats-bus.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/nats/nats-bus.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/wrapper/nats/nats-subject.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/services/config-service/config-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/services/config-service/config-service.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/schema-runner/schema-runner.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/dtos/socket-emit/socket-emit-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/dtos/socket-emit/socket-emit-dto.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/schema-runner/schema-runner.hxx
)

add_library(argus_common STATIC ${ARGUS_COMMON_SOURCES})

target_include_directories(argus_common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(argus_common
    PUBLIC
        Drogon::Drogon
        tomlplusplus::tomlplusplus
        cnats::nats_static
)

if(ARGUS_NCNN_TARGET AND TARGET ${ARGUS_NCNN_TARGET})
    target_link_libraries(argus_common PUBLIC ${ARGUS_NCNN_TARGET})
endif()

# schema-runner compiles sqlite3.h directly; the conan sqlite3 target comes
# with Drogon's dependency graph (SQLite::SQLite3 from CMakeDeps).
if(TARGET SQLite::SQLite3)
    target_link_libraries(argus_common PUBLIC SQLite::SQLite3)
endif()

foreach(_tomlplusplus_include_var
        tomlplusplus_INCLUDE_DIRS_RELEASE
        tomlplusplus_INCLUDE_DIRS_DEBUG
        tomlplusplus_INCLUDE_DIRS)
    if(DEFINED ${_tomlplusplus_include_var})
        target_include_directories(argus_common SYSTEM PUBLIC
            ${${_tomlplusplus_include_var}})
    endif()
endforeach()

target_compile_options(argus_common PRIVATE -Wall -Wextra)