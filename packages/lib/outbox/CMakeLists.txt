include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()
if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL REQUIRED)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../errors
                     ${CMAKE_BINARY_DIR}/errors)
endif()
if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../text
                     ${CMAKE_BINARY_DIR}/text)
endif()
if(NOT TARGET argus::lib::runtime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../runtime
                     ${CMAKE_BINARY_DIR}/runtime)
endif()
if(NOT TARGET argus::lib::sqlite)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sqlite
                     ${CMAKE_BINARY_DIR}/sqlite)
endif()
if(NOT TARGET argus::lib::nats)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../nats
                     ${CMAKE_BINARY_DIR}/nats)
endif()

argus_lib(NAME outbox
    SOURCES
        src/outbox/outbox-backoff.hxx
        src/outbox/outbox-key.cc
        src/outbox/outbox-key.hxx
        src/outbox/outbox-query.hxx
        src/outbox/outbox-repository.cc
        src/outbox/outbox-repository.hxx
        src/outbox/outbox-status.hxx
        src/outbox/transactional-outbox.cc
        src/outbox/transactional-outbox.hxx
    INCLUDES src
    DEPENDS
        argus::lib::errors
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_link_libraries(argus_lib_outbox PRIVATE OpenSSL::Crypto)
target_compile_options(argus_lib_outbox PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

foreach(test_name IN ITEMS outbox-key-test outbox-repository-test
                           transactional-outbox-test)
    add_executable(${test_name} tests/unit/${test_name}.cc)
    target_link_libraries(${test_name} PRIVATE argus::lib::outbox
                          doctest::doctest)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
