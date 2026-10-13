include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET cnats::nats_static)
    find_package(cnats REQUIRED)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET argus::lib::config)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../config
                     ${CMAKE_CURRENT_BINARY_DIR}/config)
endif()

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../text
                     ${CMAKE_CURRENT_BINARY_DIR}/text)
endif()

argus_lib(NAME nats
    SOURCES
        src/nats/nats-bus.cc
        src/nats/nats-bus.hxx
        src/nats/nats-subject.hxx
        src/nats/nats-permissions.cc
        src/nats/nats-permissions.hxx
        src/nats/live-broker.hxx
        src/nats/nats-push-intent-sink.hxx
        src/nats/push-intent-sink.hxx
    INCLUDES src
    DEPENDS
        cnats::nats_static
        Drogon::Drogon
        argus::lib::config
        argus::lib::text)

target_compile_options(argus_lib_nats PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(nats-wrapper-test tests/unit/nats-wrapper-test.cc)
target_link_libraries(nats-wrapper-test PRIVATE argus::lib::nats doctest::doctest)
target_compile_options(nats-wrapper-test PRIVATE -Wall -Wextra)
add_test(NAME nats-wrapper-test COMMAND nats-wrapper-test)

get_filename_component(ARGUS_NATS_DEPLOY_DIR
                       ${CMAKE_CURRENT_SOURCE_DIR}/../../../argus-deploy ABSOLUTE)
add_executable(nats-permissions-test tests/unit/nats-permissions-test.cc)
target_link_libraries(nats-permissions-test PRIVATE argus::lib::nats doctest::doctest)
target_compile_options(nats-permissions-test PRIVATE -Wall -Wextra)
target_compile_definitions(nats-permissions-test PRIVATE
    ARGUS_DEPLOY_DIR="${ARGUS_NATS_DEPLOY_DIR}"
    ARGUS_NATS_RENDER_OUT="${CMAKE_CURRENT_BINARY_DIR}/nats-permissions.conf")
add_test(NAME nats-permissions-test COMMAND nats-permissions-test)

get_filename_component(ARGUS_NATS_REPO_ROOT
                       ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
add_executable(nats-seed-round-trip-test tests/unit/nats-seed-round-trip-test.cc)
target_link_libraries(nats-seed-round-trip-test PRIVATE argus::lib::nats doctest::doctest)
target_compile_options(nats-seed-round-trip-test PRIVATE -Wall -Wextra)
add_test(NAME nats-seed-round-trip-test
         COMMAND bash ${ARGUS_NATS_REPO_ROOT}/scripts/nats-seed-round-trip-test.sh
                 $<TARGET_FILE:nats-seed-round-trip-test>)
set_tests_properties(nats-seed-round-trip-test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(nats-permissions-live-test tests/unit/nats-permissions-live-test.cc)
target_link_libraries(nats-permissions-live-test PRIVATE argus::lib::nats doctest::doctest)
target_compile_options(nats-permissions-live-test PRIVATE -Wall -Wextra)
add_test(NAME nats-permissions-live-test
         COMMAND bash ${ARGUS_NATS_REPO_ROOT}/scripts/nats-permissions-live-test.sh
                 $<TARGET_FILE:nats-permissions-live-test>)
set_tests_properties(nats-permissions-live-test PROPERTIES SKIP_RETURN_CODE 77)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
