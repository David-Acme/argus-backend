# argus_common: shared foundation library consumed by the backend, the unit
# tests, the labs and the argus-gateway service.

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

# argus::hardware-profile / argus::hardware-profile-gpu (rule 25: declared
# once, in their own folder); argus_common carries neither, so it stays
# ncnn-free for every service that links it.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/shared/wrapper/hardware-profile)

argus_module(NAME common
    SOURCES
        src/shared/enums.hxx
        src/shared/access/role-access.hxx
        src/shared/validation/rules.hxx
        src/shared/validation/validation_dsl.hxx
        src/shared/validation/validator.hxx
        src/shared/exceptions/response-exception.hxx
        src/shared/wrapper/api-response/api-response.cc
        src/shared/wrapper/api-response/api-response.hxx
        src/shared/wrapper/thread-budget/thread-budget.cc
        src/shared/wrapper/thread-budget/thread-budget.hxx
        src/shared/wrapper/blocking-task/blocking-task.hxx
        src/shared/wrapper/cancellation/cancellation-token.hxx
        src/shared/wrapper/nats/nats-bus.cc
        src/shared/wrapper/nats/nats-bus.hxx
        src/shared/wrapper/nats/nats-subject.hxx
        src/shared/services/config-service/config-service.cc
        src/shared/services/config-service/config-service.hxx
        src/shared/utils/schema-runner/schema-runner.cc
        src/shared/utils/schema-runner/schema-runner.hxx
        src/shared/dtos/socket-emit/socket-emit-dto.cc
        src/shared/dtos/socket-emit/socket-emit-dto.hxx
        src/shared/contracts/sync-operation.hxx
        src/shared/contracts/sync-filter.hxx
        src/shared/contracts/syncable.hxx
    INCLUDES src)

target_link_libraries(argus_common
    PUBLIC
        Drogon::Drogon
        tomlplusplus::tomlplusplus
        cnats::nats_static
)

# schema-runner compiles sqlite3.h directly; sqlite3 is a direct conan
# require (a transitive-only entry generates empty include dirs).
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
