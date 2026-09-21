# voice-contract: the vocabulary that crosses the voice boundary — VoiceLang,
# the language a session, a reaction rule and a stored user profile name.
# Declared once, here, for the service that owns the voice boundary and for
# the packages and services that consume it. Header-only; consumers link
# argus::contracts::voice.

cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_contracts(NAME voice
    INCLUDES src
    SOURCES
        src/voice/voice-lang.hxx)

# The round-trip suite: every name is its own spelling and back.
enable_testing()
find_package(doctest REQUIRED)

add_executable(voice-contract-vocabulary-test
    tests/unit/voice-contract-vocabulary-test.cc)
target_link_libraries(voice-contract-vocabulary-test PRIVATE
    argus::contracts::voice doctest::doctest)
target_compile_options(voice-contract-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME voice-contract-vocabulary-test
         COMMAND voice-contract-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
