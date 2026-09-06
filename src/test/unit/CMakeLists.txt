find_package(doctest REQUIRED)

set(UNIT_TESTS
    api-response-test
    enums-test
    role-access-test
    thread-budget-test
    validation-dsl-test
    nats-wrapper-test
    sync-change-test
    identity-migration-test
    camera-schema-test
    camera-migration-test
    camera-config-test
    camera-controller-test
    productivity-schema-test
    productivity-migration-test
    notification-schema-test
    notification-migration-test
    event-intelligence-test
    camera-operator-test
)

foreach(TEST_NAME IN LISTS UNIT_TESTS)
    add_executable(${TEST_NAME} ${TEST_NAME}.cc)

    target_link_libraries(${TEST_NAME} PRIVATE argus_common doctest::doctest)

    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

target_link_libraries(identity-migration-test PRIVATE identity-migration)

# camera-migration builds only as a dependency of the camera unit suites.
target_link_libraries(camera-schema-test PRIVATE camera-migration)
target_link_libraries(camera-migration-test PRIVATE camera-migration)

# productivity-migration and notification-migration build only as dependencies
# of the productivity/notification unit suites.
target_link_libraries(productivity-schema-test PRIVATE productivity-migration)
target_link_libraries(productivity-migration-test PRIVATE productivity-migration)
target_link_libraries(notification-schema-test PRIVATE notification-migration)
target_link_libraries(notification-migration-test PRIVATE notification-migration)

# camera-core (argus-camera service plumbing) is pulled into the default
# backend build only by camera-config-test and camera-controller-test.
target_link_libraries(camera-config-test PRIVATE camera-core)
target_link_libraries(camera-controller-test PRIVATE camera-core)
target_link_libraries(event-intelligence-test PRIVATE camera-core)
target_link_libraries(camera-operator-test PRIVATE camera-core)

add_executable(identity-client-test identity-client-test.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc)

target_link_libraries(identity-client-test PRIVATE argus_common Drogon::Drogon
    sqlite-vec doctest::doctest)

add_test(NAME identity-client-test COMMAND identity-client-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
