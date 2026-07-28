# Integration of bundled third-party libraries (git submodules).
# Both are built as part of this project via add_subdirectory so there is no
# external package manager involved. EXCLUDE_FROM_ALL keeps them out of a plain
# `make` and they are only built when something links against their targets.

# ---------------------------------------------------------------------------
# fastText (text classification / intent detection)
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fastText/CMakeLists.txt")
  message(STATUS "Argus: integrating fastText submodule")
  set(FASTTEXT_BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(FASTTEXT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  # fastText's CMakeLists declares cmake_minimum_required(VERSION 3.5) with the
  # deprecated single-value syntax. Bumping CMAKE_POLICY_VERSION_MINIMUM to 3.10
  # silences the "Compatibility with CMake < 3.10" deprecation warning while
  # keeping its old policies applied.
  set(ARGUS_FASTTEXT_POLICY ${CMAKE_POLICY_VERSION_MINIMUM})
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  add_subdirectory(fastText EXCLUDE_FROM_ALL)
  # fastText lowers CMAKE_CXX_STANDARD to 17 for its own targets; restore the
  # project-wide C++20 setting so the rest of Argus is unaffected.
  set(CMAKE_CXX_STANDARD 20)
  if(ARGUS_FASTTEXT_POLICY)
    set(CMAKE_POLICY_VERSION_MINIMUM ${ARGUS_FASTTEXT_POLICY})
  else()
    unset(CMAKE_POLICY_VERSION_MINIMUM)
  endif()
  if(TARGET fasttext-static_pic)
    set(ARGUS_FASTTEXT_TARGET fasttext-static_pic)
  elseif(TARGET fasttext-shared)
    set(ARGUS_FASTTEXT_TARGET fasttext-shared)
  endif()
  if(ARGUS_FASTTEXT_TARGET)
    target_include_directories(${ARGUS_FASTTEXT_TARGET} INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/fastText/src)
    message(STATUS "Argus: fastText target -> ${ARGUS_FASTTEXT_TARGET}")
    set(ARGUS_FASTTEXT_TARGET ${ARGUS_FASTTEXT_TARGET} PARENT_SCOPE)
  endif()
else()
  message(WARNING "Argus: fastText submodule not found at third_party/fastText")
endif()

# ---------------------------------------------------------------------------
# sherpa-onnx (streaming STT — Zipformer / SenseVoice via ONNX Runtime)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# ncnn (high-performance neural network inference — face detection/recognition)
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ncnn/CMakeLists.txt")
  message(STATUS "Argus: integrating ncnn submodule")
  set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(NCNN_OPENMP ON CACHE BOOL "" FORCE)
  set(NCNN_AVX2 ON CACHE BOOL "" FORCE)
  set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
  set(NCNN_ENABLE_PIC ON CACHE BOOL "" FORCE)
  add_subdirectory(ncnn EXCLUDE_FROM_ALL)
  if(TARGET ncnn)
    set(ARGUS_NCNN_TARGET ncnn)
    message(STATUS "Argus: ncnn target -> ncnn")
    set(ARGUS_NCNN_TARGET ncnn PARENT_SCOPE)
  endif()
else()
  message(WARNING "Argus: ncnn submodule not found at third_party/ncnn")
endif()

# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sherpa-onnx/CMakeLists.txt")
  message(STATUS "Argus: integrating sherpa-onnx submodule")
  # nlohmann_json is already provided by Conan — prevent sherpa-onnx
  # from downloading its own copy and creating a duplicate target
  set(json_POPULATED TRUE CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_CHECK OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_JNI OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_GPU OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
  set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
  # sherpa-onnx tries to download its own onnxruntime via FetchContent,
  # which duplicates symbols with Conan's onnxruntime. Redirect it to the
  # Conan-provided onnxruntime target instead.
  get_target_property(ARGUS_ONNX_INCLUDE_DIR onnxruntime::onnxruntime INTERFACE_INCLUDE_DIRECTORIES)
  set(location_onnxruntime_header_dir "${ARGUS_ONNX_INCLUDE_DIR}")
  set(location_onnxruntime_lib "/__sherpa_use_conan_onnxruntime__")
  set(onnxruntime_lib_files "")
  add_library(onnxruntime INTERFACE IMPORTED)
  target_include_directories(onnxruntime INTERFACE "${ARGUS_ONNX_INCLUDE_DIR}")
  target_link_libraries(onnxruntime INTERFACE onnxruntime::onnxruntime)
  add_subdirectory(sherpa-onnx EXCLUDE_FROM_ALL)
  if(TARGET sherpa-onnx-c-api)
    set(ARGUS_SHERPA_ONNX_TARGET sherpa-onnx-c-api)
    target_include_directories(sherpa-onnx-c-api INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/sherpa-onnx)
    message(STATUS "Argus: sherpa-onnx target -> sherpa-onnx-c-api")
    set(ARGUS_SHERPA_ONNX_TARGET sherpa-onnx-c-api PARENT_SCOPE)
  endif()
else()
  message(WARNING "Argus: sherpa-onnx submodule not found at third_party/sherpa-onnx")
endif()
