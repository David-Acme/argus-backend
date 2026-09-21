# notification-contract: the vocabulary that crosses the notification
# boundary — the delivery status a notification reports back through the sync
# leg. Declared once, here, for the service that owns the boundary and for the
# packages and services that consume it. Header-only; consumers link
# argus::contracts::notification.

cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_contracts(NAME notification
    INCLUDES src
    SOURCES
        src/notification/notification-delivery-status.hxx)

# The vocabulary suite: the two spellings the delivery column's CHECK accepts,
# the fallback for anything else, and the unknown-ordinal path.
enable_testing()
find_package(doctest REQUIRED)

add_executable(notification-contract-vocabulary-test
    tests/unit/notification-contract-vocabulary-test.cc)
target_link_libraries(notification-contract-vocabulary-test PRIVATE
    argus::contracts::notification doctest::doctest)
target_compile_options(notification-contract-vocabulary-test PRIVATE
    -Wall -Wextra)
add_test(NAME notification-contract-vocabulary-test
         COMMAND notification-contract-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
