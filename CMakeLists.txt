cmake_minimum_required(VERSION 3.25)
project(argus-backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR} ${CMAKE_PREFIX_PATH})

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(libsodium REQUIRED)
find_package(llama-cpp REQUIRED)

# Bundled third-party libraries (git submodules under third_party/).
add_subdirectory(third_party)

file(GLOB_RECURSE CONFIG_SRC
    ${SRC_ROOT}/config/*.cc
)

file(GLOB_RECURSE API_SRC
    ${SRC_ROOT}/feature/api/**/*.cc
)

file(GLOB_RECURSE SOCKET_SRC
    ${SRC_ROOT}/feature/socket/**/*.cc
)

file(GLOB_RECURSE FILTER_SRC
    ${SRC_ROOT}/filter/**/*.cc
)

file(GLOB_RECURSE SHARED_SRC
    ${SRC_ROOT}/shared/**/*.cc
)

add_executable(${PROJECT_NAME}
    ${SRC_ROOT}/main.cc
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${SRC_ROOT}
)

target_sources(${PROJECT_NAME}
    PRIVATE
        ${CONFIG_SRC}
        ${API_SRC}
        ${FILTER_SRC}
        ${SHARED_SRC}
        ${SOCKET_SRC}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json
        libsodium::libsodium
        llama-cpp::llama-cpp
        ${ARGUS_FASTTEXT_TARGET}
        ${CONAN_LIBS}
)

if(ARGUS_BUILD_INSPIREFACE AND TARGET InspireFace)
  target_link_libraries(${PROJECT_NAME} PRIVATE InspireFace)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.json
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/config.json
)
