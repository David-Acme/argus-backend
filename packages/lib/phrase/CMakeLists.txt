include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

argus_lib(NAME phrase
    SOURCES
        src/phrase/phrase-catalog.cc
        src/phrase/rule-parser.cc
        src/phrase/phrase-automaton.cc
    INCLUDES
        src)

target_compile_options(argus_lib_phrase PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(phrase-vocabulary-test tests/unit/phrase-vocabulary-test.cc)
target_link_libraries(phrase-vocabulary-test PRIVATE
    argus::lib::phrase doctest::doctest)
target_compile_options(phrase-vocabulary-test PRIVATE -Wall -Wextra)
add_test(NAME phrase-vocabulary-test COMMAND phrase-vocabulary-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
