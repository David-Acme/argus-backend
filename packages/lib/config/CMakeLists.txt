cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(tomlplusplus REQUIRED)
endif()

argus_lib(NAME config
    SOURCES
        src/config/service.hxx
        src/config/config-service.cc
        src/config/config-service.hxx
        src/config/settings-registry.cc
        src/config/settings-registry.hxx
    INCLUDES src
    DEPENDS
        Drogon::Drogon
        tomlplusplus::tomlplusplus)

foreach(_tomlplusplus_include_var
        tomlplusplus_INCLUDE_DIRS_RELEASE
        tomlplusplus_INCLUDE_DIRS_DEBUG
        tomlplusplus_INCLUDE_DIRS)
    if(DEFINED ${_tomlplusplus_include_var})
        target_include_directories(argus_lib_config SYSTEM PUBLIC
            ${${_tomlplusplus_include_var}})
    endif()
endforeach()

target_compile_options(argus_lib_config PRIVATE -Wall -Wextra)

enable_testing()
find_package(doctest REQUIRED)

add_executable(config-service-test tests/unit/config-service-test.cc)
target_link_libraries(config-service-test PRIVATE
    argus::lib::config
    doctest::doctest)
target_compile_options(config-service-test PRIVATE -Wall -Wextra)
add_test(NAME config-service-test COMMAND config-service-test)

add_executable(settings-registry-test tests/unit/settings-registry-test.cc)
target_link_libraries(settings-registry-test PRIVATE
    argus::lib::config
    doctest::doctest)
target_compile_options(settings-registry-test PRIVATE -Wall -Wextra)
add_test(NAME settings-registry-test COMMAND settings-registry-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()
