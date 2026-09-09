# argus_common: shared foundation library consumed by the backend, the unit
# tests, the labs and the argus-gateway service.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# s3-signing/s3-storage-service need OpenSSL; found here instead of relying
# on the caller, since not every root/standalone build path finds it before
# add_subdirectory(argus-common).
if(NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
endif()

# argus::hardware-profile / argus::hardware-profile-gpu (rule 25: declared
# once, in their own folder); argus_common carries neither, so it stays
# ncnn-free for every service that links it.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/shared/wrapper/hardware-profile)

argus_module(NAME common
    SOURCES
        src/config/app-config.cc
        src/config/app-config.hxx
        src/config/service.hxx
        src/controllers/health-controller.cc
        src/controllers/health-controller.hxx
        src/server/listener-config.cc
        src/server/listener-config.hxx
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
        src/shared/wrapper/ai-init/ai-init.hxx
        src/shared/wrapper/blocking-task/blocking-task.hxx
        src/shared/wrapper/cancellation/cancellation-token.hxx
        src/shared/wrapper/nats/nats-bus.cc
        src/shared/wrapper/nats/nats-bus.hxx
        src/shared/wrapper/nats/nats-subject.hxx
        src/shared/wrapper/sqlite-stmt/sqlite-stmt.hxx
        src/shared/services/config-service/config-service.cc
        src/shared/services/config-service/config-service.hxx
        src/shared/services/storage/s3-signing.hxx
        src/shared/services/storage/s3-storage-service.cc
        src/shared/services/storage/s3-storage-service.hxx
        src/shared/utils/json-diff/json-diff.cc
        src/shared/utils/json-diff/json-diff.hxx
        src/shared/utils/json-util/json-util.hxx
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
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
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

# The foundation suites: the envelope, the enums, the role table, the thread
# budget, the validation DSL and the NATS wrapper.
if(ARGUS_ROOT_PROJECT)
    find_package(doctest REQUIRED)

    foreach(TEST_NAME IN ITEMS api-response-test enums-test role-access-test
                               thread-budget-test validation-dsl-test
                               nats-wrapper-test)
        add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
        target_link_libraries(${TEST_NAME} PRIVATE argus_common doctest::doctest)
        target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()

    get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(DOCTEST_INCLUDE_DIRS)
        set_target_properties(doctest::doctest PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
    endif()
endif()
