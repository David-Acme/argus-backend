# Integration of bundled third-party libraries (git submodules).
# Both are built as part of this project via add_subdirectory so there is no
# external package manager involved. EXCLUDE_FROM_ALL keeps them out of a plain
# `make` and they are only built when something links against their targets.

# ---------------------------------------------------------------------------
# fastText (text classification / intent detection — IntentService)
# Pinned at 1f12150 (v0.9.2 + local "build with C++20" patch).
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
    # fastText predates C++20: silence the implicit-this-capture deprecation
    # warning that only appears when compiled with the project's C++20 patch.
    target_compile_options(fasttext-static_pic PRIVATE -Wno-deprecated)
  elseif(TARGET fasttext-shared)
    set(ARGUS_FASTTEXT_TARGET fasttext-shared)
  endif()
  if(ARGUS_FASTTEXT_TARGET)
    target_include_directories(${ARGUS_FASTTEXT_TARGET} SYSTEM INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/fastText/src)
    message(STATUS "Argus: fastText target -> ${ARGUS_FASTTEXT_TARGET}")
    set(ARGUS_FASTTEXT_TARGET ${ARGUS_FASTTEXT_TARGET} PARENT_SCOPE)
  endif()
else()
  message(WARNING "Argus: fastText submodule not found at third_party/fastText")
endif()

# ---------------------------------------------------------------------------
# Kùzu (embedded graph DB — upstream kuzudb/kuzu @ v0.11.3, the final release
# before the project was archived on 2025-10-10; Vela fork pinned at
# third_party/kuzu-vela as a future upgrade path). GATE RESULT: kuzu does NOT
# become Argus' memory engine. Measured in Release @ 5 000 facts: build
# offline OK, RSS 54 MB, KNN p95 13 ms, traversal p95 2 ms — but the real
# access pattern (consolidation worker inserting while recall reads, single
# serialized connection, COGNITIVE_MEMORY_PLAN.md §9) FAILS: inserts degrade
# from ~12 ms to ~9 s under concurrent reads (checkpoint starvation), with
# non-deterministic DirectedCSRIndex asserts and SIGTERM-proof hangs. The
# plan's fallback applies: SemanticGraph backed by the existing SQLite
# tables. Both submodules stay pinned purely as the reproducer/upgrade path:
# labs/kuzu-probe (EXCLUDE_FROM_ALL — never built by default, ~40 min
# compile) is the gate probe and the crash reproducer for the Vela fork.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/kuzu/CMakeLists.txt")
  message(STATUS "Argus: integrating kuzu submodule (reproducer only — gate failed)")

  set(BUILD_KUZU ON CACHE BOOL "" FORCE)
  set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_EXTENSION_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
  set(BUILD_NODEJS OFF CACHE BOOL "" FORCE)
  set(BUILD_PYTHON OFF CACHE BOOL "" FORCE)
  set(BUILD_SINGLE_FILE_HEADER OFF CACHE BOOL "" FORCE)
  set(ENABLE_BACKTRACES OFF CACHE BOOL "" FORCE)
  set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  # Bundled, statically linked, auto-registered at Database construction:
  # no INSTALL/LOAD step, no extension registry reachable over the network.
  set(BUILD_EXTENSIONS "fts;vector" CACHE STRING "" FORCE)
  set(EXTENSION_STATIC_LINK_LIST "fts;vector" CACHE STRING "" FORCE)

  add_subdirectory(kuzu EXCLUDE_FROM_ALL)

  if(TARGET kuzu)
    set(ARGUS_KUZU_TARGET kuzu)
    set(ARGUS_KUZU_TARGET kuzu PARENT_SCOPE)
    message(STATUS "Argus: kuzu target -> kuzu")
  else()
    message(FATAL_ERROR "Argus: kuzu built but kuzu target missing")
  endif()
else()
  message(WARNING "Argus: kuzu submodule not found at third_party/kuzu")
endif()

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
