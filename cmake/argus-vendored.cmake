include_guard(GLOBAL)

set(ARGUS_VENDORED_RECIPE 1)

set(argus_vendored_default_root "")
if(DEFINED ENV{ARGUS_VENDORED_ROOT} AND NOT "$ENV{ARGUS_VENDORED_ROOT}" STREQUAL "")
  set(argus_vendored_default_root "$ENV{ARGUS_VENDORED_ROOT}")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
  set(argus_vendored_default_root "$ENV{XDG_CACHE_HOME}/argus/vendored")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(argus_vendored_default_root "$ENV{HOME}/.cache/argus/vendored")
else()
  set(argus_vendored_default_root "${CMAKE_BINARY_DIR}/argus-vendored")
endif()
set(ARGUS_VENDORED_ROOT "${argus_vendored_default_root}" CACHE PATH
    "argus: where vendored engines are built once and reused by every build tree")

set(ARGUS_GGML_GPU "AUTO" CACHE STRING
    "argus: ggml GPU backend (AUTO, OFF, METAL, VULKAN, CUDA)")
set_property(CACHE ARGUS_GGML_GPU PROPERTY STRINGS AUTO OFF METAL VULKAN CUDA)

set(ARGUS_NCNN_GPU "AUTO" CACHE STRING "argus: ncnn Vulkan compute (AUTO, OFF, VULKAN)")
set_property(CACHE ARGUS_NCNN_GPU PROPERTY STRINGS AUTO OFF VULKAN)

function(argus_vendored_source_id source out_var)
  find_package(Git QUIET)
  if(GIT_EXECUTABLE AND EXISTS "${source}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${source}" rev-parse HEAD
      OUTPUT_VARIABLE head RESULT_VARIABLE head_result
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${source}" status --porcelain --untracked-files=no
      OUTPUT_VARIABLE dirty RESULT_VARIABLE dirty_result
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(head_result EQUAL 0 AND dirty_result EQUAL 0 AND dirty STREQUAL "")
      set(${out_var} "git:${head}" PARENT_SCOPE)
      return()
    endif()
  endif()
  file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${source}" "${source}/*")
  list(FILTER files EXCLUDE REGEX "^\\.git(/|$)")
  list(SORT files)
  set(digest "")
  foreach(file IN LISTS files)
    file(SHA256 "${source}/${file}" file_hash)
    string(APPEND digest "${file} ${file_hash}\n")
  endforeach()
  string(SHA256 tree "${digest}")
  set(${out_var} "tree:${tree}" PARENT_SCOPE)
endfunction()

function(argus_vendored_jobs out_var)
  if(DEFINED ENV{ARGUS_VENDORED_JOBS} AND "$ENV{ARGUS_VENDORED_JOBS}" MATCHES "^[1-9][0-9]*$")
    set(${out_var} "$ENV{ARGUS_VENDORED_JOBS}" PARENT_SCOPE)
  elseif(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL}
         AND "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" MATCHES "^[1-9][0-9]*$")
    set(${out_var} "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" PARENT_SCOPE)
  else()
    cmake_host_system_information(RESULT cores QUERY NUMBER_OF_LOGICAL_CORES)
    set(${out_var} "${cores}" PARENT_SCOPE)
  endif()
endfunction()

function(argus_vendored_build)
  cmake_parse_arguments(ARG "" "NAME;SOURCE;PREFIX_VAR" "OPTIONS" ${ARGN})
  if(NOT ARG_NAME OR NOT ARG_SOURCE OR NOT ARG_PREFIX_VAR)
    message(FATAL_ERROR "argus_vendored_build requires NAME, SOURCE and PREFIX_VAR")
  endif()
  cmake_path(ABSOLUTE_PATH ARG_SOURCE NORMALIZE)
  if(NOT EXISTS "${ARG_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR
            "argus_vendored_build: ${ARG_SOURCE} has no CMakeLists.txt; run git submodule update --init")
  endif()

  argus_vendored_source_id("${ARG_SOURCE}" source_id)
  set(build_type "${CMAKE_BUILD_TYPE}")
  if(NOT build_type)
    set(build_type Release)
  endif()
  string(TOUPPER "${build_type}" build_config)
  list(APPEND ARG_OPTIONS
       "-DCMAKE_BUILD_TYPE=${build_type}"
       "-DCMAKE_CXX_FLAGS_${build_config}=${CMAKE_CXX_FLAGS_${build_config}}"
       "-DCMAKE_C_FLAGS_${build_config}=${CMAKE_CXX_FLAGS_${build_config}}")
  if(APPLE)
    list(APPEND ARG_OPTIONS "-DCMAKE_INSTALL_RPATH=@loader_path")
  else()
    list(APPEND ARG_OPTIONS "-DCMAKE_INSTALL_RPATH=\$ORIGIN")
  endif()
  set(compiler_c "")
  if(DEFINED ENV{CC} AND NOT "$ENV{CC}" STREQUAL "")
    set(compiler_c "$ENV{CC}")
  endif()
  string(JOIN "\n" material
         "recipe=${ARGUS_VENDORED_RECIPE}"
         "name=${ARG_NAME}"
         "source=${source_id}"
         "system=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
         "cxx=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_CXX_COMPILER}"
         "cc=${compiler_c}"
         "options=${ARG_OPTIONS}")
  string(SHA256 key "${material}")
  string(SUBSTRING "${key}" 0 16 key)
  string(TOLOWER "${build_type}" build_suffix)
  set(prefix "${ARGUS_VENDORED_ROOT}/${ARG_NAME}-${build_suffix}-${key}")
  set(stamp "${prefix}/.argus-complete")

  if(NOT EXISTS "${stamp}")
    file(MAKE_DIRECTORY "${ARGUS_VENDORED_ROOT}")
    file(LOCK "${prefix}.lock" GUARD FUNCTION
         TIMEOUT 10800 RESULT_VARIABLE lock_result)
    if(NOT lock_result EQUAL 0)
      message(FATAL_ERROR "argus_vendored_build: cannot lock ${ARG_NAME}: ${lock_result}")
    endif()
  endif()

  if(NOT EXISTS "${stamp}")
    argus_vendored_jobs(jobs)
    cmake_path(GET prefix FILENAME prefix_name)
    set(work "${ARGUS_VENDORED_ROOT}/.work/${prefix_name}")
    set(log "${ARGUS_VENDORED_ROOT}/.work/${prefix_name}.log")
    file(REMOVE_RECURSE "${work}" "${prefix}")
    file(MAKE_DIRECTORY "${work}")
    set(launcher "")
    find_program(argus_vendored_ccache NAMES ccache)
    if(argus_vendored_ccache)
      set(launcher
          "-DCMAKE_C_COMPILER_LAUNCHER=${argus_vendored_ccache}"
          "-DCMAKE_CXX_COMPILER_LAUNCHER=${argus_vendored_ccache}")
    endif()
    set(compilers "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    if(compiler_c)
      list(APPEND compilers "-DCMAKE_C_COMPILER=${compiler_c}")
    endif()
    message(STATUS
            "argus vendored: building ${ARG_NAME} once into ${prefix} (${jobs} jobs, log ${log})")
    foreach(step configure build install)
      if(step STREQUAL "configure")
        set(command "${CMAKE_COMMAND}" -S "${ARG_SOURCE}" -B "${work}" -G Ninja
            "-DCMAKE_INSTALL_PREFIX=${prefix}"
            -DCMAKE_INSTALL_LIBDIR=lib
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            ${compilers} ${launcher} ${ARG_OPTIONS})
      elseif(step STREQUAL "build")
        set(command "${CMAKE_COMMAND}" --build "${work}" -j "${jobs}")
      else()
        set(command "${CMAKE_COMMAND}" --install "${work}")
      endif()
      execute_process(COMMAND ${command}
                      RESULT_VARIABLE step_result
                      OUTPUT_FILE "${log}.${step}" ERROR_FILE "${log}.${step}")
      if(NOT step_result EQUAL 0)
        file(STRINGS "${log}.${step}" tail_lines)
        list(LENGTH tail_lines line_count)
        math(EXPR first "${line_count} - 40")
        if(first LESS 0)
          set(first 0)
        endif()
        list(SUBLIST tail_lines ${first} -1 tail_lines)
        list(JOIN tail_lines "\n" tail_text)
        message(FATAL_ERROR
                "argus vendored: ${ARG_NAME} ${step} failed (${step_result}); full log ${log}.${step}\n${tail_text}")
      endif()
    endforeach()
    file(WRITE "${prefix}/.argus-material" "${material}\n")
    file(TOUCH "${stamp}")
    file(REMOVE_RECURSE "${work}")
    file(REMOVE "${log}.configure" "${log}.build" "${log}.install")
  else()
    message(STATUS "argus vendored: ${ARG_NAME} reused from ${prefix}")
  endif()

  set(${ARG_PREFIX_VAR} "${prefix}" PARENT_SCOPE)
endfunction()

function(argus_vendored_ggml_backend out_options)
  set(choice "${ARGUS_GGML_GPU}")
  if(choice STREQUAL "AUTO")
    if(APPLE)
      set(choice METAL)
    else()
      include(CheckLanguage)
      check_language(CUDA)
      if(CMAKE_CUDA_COMPILER)
        set(choice CUDA)
      else()
        find_package(Vulkan QUIET COMPONENTS glslc)
        find_package(SPIRV-Headers CONFIG QUIET)
        if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE AND SPIRV-Headers_FOUND)
          set(choice VULKAN)
        else()
          set(choice OFF)
        endif()
      endif()
    endif()
  endif()
  set(options -DGGML_CUDA=OFF -DGGML_VULKAN=OFF)
  if(APPLE)
    list(APPEND options -DGGML_OPENMP=OFF -DGGML_ACCELERATE=ON)
  else()
    list(APPEND options -DGGML_OPENMP=ON)
  endif()
  if(choice STREQUAL "METAL")
    list(APPEND options -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON)
  elseif(APPLE)
    list(APPEND options -DGGML_METAL=OFF)
  endif()
  if(choice STREQUAL "VULKAN")
    list(REMOVE_ITEM options -DGGML_VULKAN=OFF)
    list(APPEND options -DGGML_VULKAN=ON)
  elseif(choice STREQUAL "CUDA")
    list(REMOVE_ITEM options -DGGML_CUDA=OFF)
    list(APPEND options -DGGML_CUDA=ON)
  endif()
  message(STATUS "argus ggml backend: ${choice} (ARGUS_GGML_GPU=${ARGUS_GGML_GPU})")
  set(${out_options} ${options} PARENT_SCOPE)
endfunction()

function(argus_vendored_copy_runtime prefix)
  file(GLOB runtime_libs LIST_DIRECTORIES false
       "${prefix}/lib/*.so" "${prefix}/lib/*.so.*" "${prefix}/lib/*.dylib")
  if(runtime_libs)
    file(COPY ${runtime_libs} DESTINATION "${CMAKE_BINARY_DIR}/bin" FOLLOW_SYMLINK_CHAIN)
  endif()
endfunction()

function(argus_vendored_relocate target libdir)
  get_target_property(argus_type ${target} TYPE)
  if(NOT argus_type MATCHES "^(SHARED|UNKNOWN)_LIBRARY$")
    return()
  endif()
  set(argus_properties IMPORTED_LOCATION)
  get_target_property(argus_configs ${target} IMPORTED_CONFIGURATIONS)
  if(argus_configs)
    foreach(argus_config IN LISTS argus_configs)
      list(APPEND argus_properties IMPORTED_LOCATION_${argus_config})
    endforeach()
  endif()
  foreach(argus_property IN LISTS argus_properties)
    get_target_property(argus_location ${target} ${argus_property})
    string(FIND "${argus_location}" "${libdir}/" argus_at)
    if(argus_location AND argus_at EQUAL 0)
      get_filename_component(argus_name "${argus_location}" NAME)
      if(EXISTS "${CMAKE_BINARY_DIR}/bin/${argus_name}")
        set_target_properties(${target} PROPERTIES ${argus_property} "${CMAKE_BINARY_DIR}/bin/${argus_name}")
      endif()
    endif()
  endforeach()
endfunction()

macro(argus_vendored_llama)
  if(NOT TARGET llama)
    argus_vendored_ggml_backend(argus_llama_backend)
    argus_vendored_build(NAME llama
        SOURCE ${ARGUS_REPO_ROOT}/third_party/llama.cpp
        PREFIX_VAR argus_llama_prefix
        OPTIONS
            -DBUILD_SHARED_LIBS=ON
            -DLLAMA_BUILD_COMMON=OFF
            -DLLAMA_BUILD_TESTS=OFF
            -DLLAMA_BUILD_TOOLS=OFF
            -DLLAMA_BUILD_EXAMPLES=OFF
            -DLLAMA_BUILD_SERVER=OFF
            -DLLAMA_BUILD_APP=OFF
            -DLLAMA_BUILD_UI=OFF
            -DLLAMA_USE_PREBUILT_UI=OFF
            -DLLAMA_CURL=OFF
            -DLLAMA_BUILD_MTMD=ON
            -DMTMD_VIDEO=OFF
            -DGGML_NATIVE=ON
            -DGGML_CCACHE=OFF
            -DGGML_BUILD_TESTS=OFF
            -DGGML_BUILD_EXAMPLES=OFF
            ${argus_llama_backend})
    foreach(argus_stale IN ITEMS llama_DIR ggml_DIR llama_LIBRARY GGML_LIBRARY GGML_BASE_LIBRARY
                                 GGML_CPU_LIBRARY GGML_VULKAN_LIBRARY GGML_CUDA_LIBRARY
                                 GGML_METAL_LIBRARY GGML_BLAS_LIBRARY)
      unset(${argus_stale} CACHE)
    endforeach()
    find_package(llama CONFIG REQUIRED PATHS "${argus_llama_prefix}" NO_DEFAULT_PATH)
    find_library(argus_mtmd_library mtmd REQUIRED
                 PATHS "${argus_llama_prefix}/lib" NO_DEFAULT_PATH)
    add_library(mtmd SHARED IMPORTED)
    set_target_properties(mtmd PROPERTIES
        IMPORTED_LOCATION "${argus_mtmd_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${argus_llama_prefix}/include"
        INTERFACE_LINK_LIBRARIES llama)
    set_target_properties(llama PROPERTIES
        INTERFACE_COMPILE_FEATURES ""
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${argus_llama_prefix}/include")
    get_property(argus_llama_imported DIRECTORY PROPERTY IMPORTED_TARGETS)
    foreach(argus_ggml_target IN LISTS argus_llama_imported)
      if(argus_ggml_target MATCHES "^ggml")
        set_target_properties(${argus_ggml_target} PROPERTIES INTERFACE_COMPILE_FEATURES "")
      endif()
    endforeach()
    argus_vendored_copy_runtime("${argus_llama_prefix}")
    get_property(argus_llama_imported DIRECTORY PROPERTY IMPORTED_TARGETS)
    foreach(argus_runtime_target IN LISTS argus_llama_imported)
      if(argus_runtime_target MATCHES "^(llama|mtmd|ggml)")
        argus_vendored_relocate(${argus_runtime_target} "${argus_llama_prefix}/lib")
      endif()
    endforeach()
    unset(argus_mtmd_library CACHE)
  endif()
endmacro()

macro(argus_vendored_ncnn)
  if(NOT TARGET ncnn)
    set(argus_ncnn_choice "${ARGUS_NCNN_GPU}")
    if(argus_ncnn_choice STREQUAL "AUTO")
      if(APPLE)
        set(argus_ncnn_choice OFF)
      else()
        set(argus_ncnn_choice VULKAN)
      endif()
    endif()
    set(argus_ncnn_options
        -DNCNN_BUILD_BENCHMARK=OFF
        -DNCNN_BUILD_EXAMPLES=OFF
        -DNCNN_BUILD_TOOLS=OFF
        -DNCNN_BUILD_TESTS=OFF
        -DNCNN_SHARED_LIB=OFF
        -DNCNN_ENABLE_PIC=ON)
    if(APPLE)
      list(APPEND argus_ncnn_options -DNCNN_OPENMP=OFF)
    else()
      list(APPEND argus_ncnn_options -DNCNN_OPENMP=ON -DNCNN_AVX2=ON)
    endif()
    if(argus_ncnn_choice STREQUAL "VULKAN")
      list(APPEND argus_ncnn_options -DNCNN_VULKAN=ON)
    else()
      list(APPEND argus_ncnn_options -DNCNN_VULKAN=OFF)
    endif()
    message(STATUS "argus ncnn GPU: ${argus_ncnn_choice} (ARGUS_NCNN_GPU=${ARGUS_NCNN_GPU})")
    argus_vendored_build(NAME ncnn
        SOURCE ${ARGUS_REPO_ROOT}/third_party/ncnn
        PREFIX_VAR argus_ncnn_prefix
        OPTIONS ${argus_ncnn_options})
    unset(ncnn_DIR CACHE)
    unset(glslang_DIR CACHE)
    find_package(ncnn CONFIG REQUIRED PATHS "${argus_ncnn_prefix}" NO_DEFAULT_PATH)
  endif()
  set(ARGUS_NCNN_TARGET ncnn)
endmacro()
