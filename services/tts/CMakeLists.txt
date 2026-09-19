cmake_minimum_required(VERSION 3.25)
project(argus-tts VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_contracts_substrate()

set(TTS_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure): add the
    # sibling packages the tts module links, argus-common first.
    find_package(Drogon CONFIG REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/common
                     ${CMAKE_BINARY_DIR}/common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts-client
                     ${CMAKE_BINARY_DIR}/tts-client)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/auth
                     ${CMAKE_BINARY_DIR}/auth)
    # The synthesis feature sizes its pool from argus::hardware-profile, which
    # used to arrive through argus-common; it is its own package now.
    if(NOT TARGET argus::hardware-profile)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/hardware/src/shared/wrapper/hardware-profile
                         ${CMAKE_BINARY_DIR}/hardware-profile)
    endif()
endif()

if(NOT TARGET argus::tts-client)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts-client
                     ${CMAKE_BINARY_DIR}/tts-client)
endif()

find_package(doctest REQUIRED)

file(GLOB TTS_FEATURE_MODULES CONFIGURE_DEPENDS
    "${TTS_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS TTS_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/app/rpc)

argus_service(NAME argus-tts
    MAIN ${TTS_SRC_ROOT}/app/main.cc
    MODULES argus::tts-rpc argus::tts-synthesis-http)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set_target_properties(argus-tts PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
argus_runtime_rpath(argus-tts)

add_custom_command(TARGET argus-tts POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models
            $<TARGET_FILE_DIR:argus-tts>/models)

enable_testing()

add_executable(tts-wire-test tests/unit/tts-wire-test.cc)

target_compile_definitions(tts-wire-test PRIVATE
    ARGUS_TEST_TTS_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/tts")

target_link_libraries(tts-wire-test PRIVATE
    argus::tts-synthesis-http doctest::doctest)

target_compile_options(tts-wire-test PRIVATE -Wall -Wextra)

set_target_properties(tts-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME tts-wire-test COMMAND tts-wire-test)

add_executable(tts-rpc-test tests/unit/tts-rpc-test.cc)

target_link_libraries(tts-rpc-test PRIVATE argus::tts-rpc argus::tts-synthesis doctest::doctest)
target_compile_definitions(tts-rpc-test PRIVATE
    ARGUS_TEST_TTS_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/tts")
argus_runtime_rpath(tts-rpc-test)

target_compile_options(tts-rpc-test PRIVATE -Wall -Wextra)

set_target_properties(tts-rpc-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME tts-rpc-test COMMAND tts-rpc-test)
