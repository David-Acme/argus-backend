# nats: the NATS event bus wrapper (connections, subjects, push-intent sink)
# every service publishes and consumes through. Consumers link argus::lib::nats.

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

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
