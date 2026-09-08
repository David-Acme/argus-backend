find_package(doctest REQUIRED)

# Transitional: this tree still compiles shared/config/filter sources by raw
# path (rule-25 debt, tracked for the move to per-service tests/), so it
# needs the whole src/ root itself now that argus_common no longer exports
# it; see argus-common/CONTEXT.md.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(UNIT_TESTS
    api-response-test
    enums-test
    role-access-test
    thread-budget-test
    validation-dsl-test
    nats-wrapper-test
    sync-change-test
    cert-san-test
)

foreach(TEST_NAME IN LISTS UNIT_TESTS)
    add_executable(${TEST_NAME} ${TEST_NAME}.cc)

    target_link_libraries(${TEST_NAME} PRIVATE argus_common doctest::doctest)

    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# The cert SAN suite rides the shared identity domain too: CertService rotation
# and the in-process TLS listener that proves the hot reload.
target_link_libraries(cert-san-test PRIVATE argus_identity)


add_executable(event-sync-empty-test event-sync-empty-test.cc)

target_link_libraries(event-sync-empty-test PRIVATE argus_sync argus::sqlite
    argus_common Drogon::Drogon sqlite-vec doctest::doctest)

add_test(NAME event-sync-empty-test COMMAND event-sync-empty-test)

add_executable(identity-client-test identity-client-test.cc)

target_link_libraries(identity-client-test PRIVATE argus::sqlite argus_common
    Drogon::Drogon sqlite-vec doctest::doctest)

add_test(NAME identity-client-test COMMAND identity-client-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()




# The argus-llm wire contract (F4-5, Rulings BT/BS): the real engine serves
# the chat/stream/config legs from the shared models tree, with the sentinel
