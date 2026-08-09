# Integration of bundled third-party libraries (git submodules).
# Both are built as part of this project via add_subdirectory so there is no
# external package manager involved. EXCLUDE_FROM_ALL keeps them out of a plain
# `make` and they are only built when something links against their targets.

# ---------------------------------------------------------------------------
# sqlite-vec (vendored, MIT/Apache-2.0) — vec0 vector search for SQLite.
# Compiled with SQLITE_CORE so it links statically against Drogon's sqlite3
# (it only defines vec0_*/sqlite3_vec_init symbols — no clash).
# ---------------------------------------------------------------------------
enable_language(C)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sqlite-vec/sqlite-vec.c")
  add_library(sqlite-vec STATIC sqlite-vec/sqlite-vec.c)
  target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
  target_include_directories(sqlite-vec PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/sqlite-vec)
  set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)
  set(ARGUS_SQLITE_VEC_TARGET sqlite-vec PARENT_SCOPE)
  message(STATUS "Argus: sqlite-vec target -> sqlite-vec")
else()
  message(WARNING "Argus: sqlite-vec not found at third_party/sqlite-vec")
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

# llama.cpp (LLM inference + libmtmd multimodal). Vendored instead of taken
# from Conan: llama-cpp/b6565 is the newest recipe on Conan Center and its
# lfm2 projector still requires mm.input_norm.*, which LFM2.5-VL dropped, so
# mtmd_init_from_file() rejects the newer mmproj files. Conan also ships a
# CPU-only build with no mtmd component exposed.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp/CMakeLists.txt")
  message(STATUS "Argus: integrating llama.cpp submodule")

  set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
  set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)
  set(MTMD_VIDEO OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE ON CACHE BOOL "" FORCE)
  set(GGML_OPENMP ON CACHE BOOL "" FORCE)
  set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

  # Backend precedence: CUDA > Vulkan > CPU. On NVIDIA the CUDA backend is
  # substantially faster than the generic Vulkan path, so it wins when the
  # toolkit is present. Missing GPU support is never fatal: the CPU backend
  # runs everywhere, so the build degrades instead of failing.
  set(GGML_CUDA OFF CACHE BOOL "" FORCE)
  set(GGML_VULKAN OFF CACHE BOOL "" FORCE)

  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
    message(STATUS "Argus: llama.cpp CUDA backend ON (${CMAKE_CUDA_COMPILER})")
  else()
    find_package(Vulkan QUIET COMPONENTS glslc)
    find_package(SPIRV-Headers CONFIG QUIET)
    if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE AND SPIRV-Headers_FOUND)
      set(GGML_VULKAN ON CACHE BOOL "" FORCE)
      message(STATUS "Argus: llama.cpp Vulkan backend ON (${Vulkan_GLSLC_EXECUTABLE})")
    elseif(NOT Vulkan_FOUND OR NOT Vulkan_GLSLC_EXECUTABLE)
      message(STATUS "Argus: llama.cpp GPU backends OFF (no CUDA, no Vulkan/glslc)")
    else()
      message(STATUS
          "Argus: llama.cpp GPU backends OFF (SPIRV-Headers not installed). "
          "Install it to offload the LLM/VLM to the GPU.")
    endif()
  endif()

  add_subdirectory(llama.cpp EXCLUDE_FROM_ALL)

  if(TARGET llama AND TARGET mtmd)
    set(ARGUS_LLAMA_TARGETS llama mtmd)
    set(ARGUS_LLAMA_TARGETS llama mtmd PARENT_SCOPE)
    message(STATUS "Argus: llama.cpp targets -> llama mtmd")
  else()
    message(FATAL_ERROR "Argus: llama.cpp built but llama/mtmd targets missing")
  endif()
else()
  message(FATAL_ERROR
      "Argus: llama.cpp submodule not found at third_party/llama.cpp. "
      "Run: git submodule update --init --recursive")
endif()
