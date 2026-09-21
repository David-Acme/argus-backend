cmake_minimum_required(VERSION 3.25)
project(argus-tts VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_contracts_substrate()

set(TTS_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET Drogon::Drogon)
    # Standalone configure: this service owns the whole build, so it adds
    # the packages it links — the leaves it uses directly and the sibling
    # packages that used to arrive through argus-common.
    find_package(Drogon CONFIG REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    # The synthesis feature sizes its pool from the hardware profile, and the
    # service's coroutines from the blocking-task awaiter: both live here.
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    # The synthesis HTTP DTOs validate through argus::lib::validation.
    if(NOT TARGET argus::lib::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::clients::tts)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts
                         ${CMAKE_BINARY_DIR}/clients/tts)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
endif()

if(NOT TARGET argus::clients::tts)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts
                     ${CMAKE_BINARY_DIR}/clients/tts)
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
