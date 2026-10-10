include_guard(GLOBAL)

set(ARGUS_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})

set(argus_link_budget_mb 4096)
find_program(ARGUS_MOLD_EXECUTABLE NAMES mold)
if(ARGUS_MOLD_EXECUTABLE AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
  set(CMAKE_LINKER_TYPE MOLD)
  set(argus_link_budget_mb 3072)
  message(STATUS
          "argus linker: mold (${ARGUS_MOLD_EXECUTABLE}), link budget ${argus_link_budget_mb} MiB")
else()
  message(STATUS "argus linker: default, link budget ${argus_link_budget_mb} MiB")
endif()

option(ARGUS_CCACHE "argus: cache compiler output with ccache when it is installed" ON)
set(GGML_CCACHE OFF CACHE BOOL "ggml: use ccache if available" FORCE)

find_program(ARGUS_CCACHE_EXECUTABLE NAMES ccache)
if(ARGUS_CCACHE AND ARGUS_CCACHE_EXECUTABLE)
  set(argus_ccache_basedir "")
  if(ARGUS_CCACHE_BASEDIR)
    set(argus_ccache_basedir "${ARGUS_CCACHE_BASEDIR}")
  elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(argus_ccache_basedir "$ENV{HOME}")
  endif()
  set(argus_ccache_env CCACHE_NOHASHDIR=1 CCACHE_MAXSIZE=15G)
  if(argus_ccache_basedir)
    list(PREPEND argus_ccache_env "CCACHE_BASEDIR=${argus_ccache_basedir}")
  endif()
  set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env ${argus_ccache_env}
                                 ${ARGUS_CCACHE_EXECUTABLE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env ${argus_ccache_env}
                                   ${ARGUS_CCACHE_EXECUTABLE})
  message(STATUS "argus compiler launcher: ccache (${ARGUS_CCACHE_EXECUTABLE}, base_dir ${argus_ccache_basedir})")
else()
  message(STATUS "argus compiler launcher: none")
endif()

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
message(STATUS "argus C++20 module scanning: off")

if(CMAKE_GENERATOR MATCHES "Ninja" AND NOT CMAKE_JOB_POOLS)
  set(argus_reserve_mb 4096)
  set(argus_cap_mb 0)
  set(argus_cap_source "")
  set(argus_link_pool 0)
  if(DEFINED ARGUS_LINK_POOLS AND ARGUS_LINK_POOLS MATCHES "^[0-9]+$"
     AND ARGUS_LINK_POOLS GREATER 0)
    set(argus_link_pool "${ARGUS_LINK_POOLS}")
  elseif(DEFINED ENV{ARGUS_LINK_POOLS}
         AND "$ENV{ARGUS_LINK_POOLS}" MATCHES "^[0-9]+$"
         AND "$ENV{ARGUS_LINK_POOLS}" GREATER 0)
    set(argus_link_pool "$ENV{ARGUS_LINK_POOLS}")
  else()
    if(DEFINED ARGUS_BUILD_MEMORY_CAP_MB
       AND ARGUS_BUILD_MEMORY_CAP_MB MATCHES "^[0-9]+$")
      set(argus_cap_mb "${ARGUS_BUILD_MEMORY_CAP_MB}")
      set(argus_cap_source "memory cap ${argus_cap_mb} MiB")
    elseif(DEFINED ENV{ARGUS_BUILD_MEMORY_CAP_MB}
           AND "$ENV{ARGUS_BUILD_MEMORY_CAP_MB}" MATCHES "^[0-9]+$")
      set(argus_cap_mb "$ENV{ARGUS_BUILD_MEMORY_CAP_MB}")
      set(argus_cap_source "memory cap ${argus_cap_mb} MiB")
    endif()
    if(argus_cap_mb EQUAL 0 AND EXISTS "/proc/self/cgroup")
      file(READ /proc/self/cgroup argus_cgroup)
      string(REGEX MATCH "0::([^\n\r]*)" argus_cgroup_hit "${argus_cgroup}")
      set(argus_cgroup_dir "")
      if(CMAKE_MATCH_1)
        set(argus_cgroup_dir "/sys/fs/cgroup${CMAKE_MATCH_1}")
      endif()
      if(argus_cgroup_dir AND EXISTS "${argus_cgroup_dir}/memory.max")
        file(READ "${argus_cgroup_dir}/memory.max" argus_memory_max)
        string(STRIP "${argus_memory_max}" argus_memory_max)
        if(argus_memory_max MATCHES "^[0-9]+$")
          math(EXPR argus_cap_mb "${argus_memory_max} / 1048576")
          set(argus_cap_source "cgroup memory.max ${argus_cap_mb} MiB")
        endif()
      endif()
    endif()
    if(argus_cap_mb EQUAL 0 AND EXISTS "/proc/meminfo")
      file(READ /proc/meminfo argus_meminfo)
      string(REGEX MATCH "MemAvailable:[ \t]+([0-9]+) kB" argus_meminfo_hit
                   "${argus_meminfo}")
      if(CMAKE_MATCH_1)
        math(EXPR argus_avail_mb "${CMAKE_MATCH_1} / 1024")
        math(EXPR argus_cap_mb "${argus_avail_mb} - ${argus_reserve_mb}")
        set(argus_cap_source
            "MemAvailable ${argus_avail_mb} MiB - reserve ${argus_reserve_mb} MiB")
      endif()
    endif()
    if(argus_cap_mb GREATER 0)
      math(EXPR argus_link_pool "${argus_cap_mb} / ${argus_link_budget_mb}")
    endif()
  endif()
  if(argus_link_pool LESS 1)
    set(argus_link_pool 1)
  endif()
  set(CMAKE_JOB_POOLS "link=${argus_link_pool}")
  set(CMAKE_JOB_POOL_LINK link)
  if(argus_cap_source)
    message(STATUS "argus link pool: link=${argus_link_pool} (${argus_cap_source})")
  else()
    message(STATUS "argus link pool: link=${argus_link_pool}")
  endif()
endif()

macro(argus_reject_unknown_args helper)
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
            "${helper}: unknown argument(s) '${ARG_UNPARSED_ARGUMENTS}'")
  endif()
  foreach(list IN ITEMS ARG_SOURCES ARG_INCLUDES ARG_DEPENDS ARG_SYSTEM_DEPENDS
                         ARG_PROTO ARG_MODULES)
    foreach(item IN LISTS ${list})
      if(item MATCHES "^[A-Z][A-Z0-9_]+$")
        message(FATAL_ERROR
                "${helper}: '${item}' in ${list} looks like a misspelled keyword")
      endif()
    endforeach()
  endforeach()
endmacro()

function(argus_grouped_module)
  cmake_parse_arguments(ARG "INTERFACE" "NAME;GROUP"
                        "SOURCES;INCLUDES;DEPENDS;SYSTEM_DEPENDS" ${ARGN})
  argus_reject_unknown_args(argus_grouped_module)
  if(NOT ARG_NAME)
    message(FATAL_ERROR "argus_grouped_module requires NAME")
  endif()
  set(abs_sources "")
  foreach(src IN LISTS ARG_SOURCES)
    cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
               NORMALIZE)
    list(APPEND abs_sources ${src})
  endforeach()
  set(abs_includes "")
  foreach(inc IN LISTS ARG_INCLUDES)
    cmake_path(ABSOLUTE_PATH inc BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
               NORMALIZE)
    list(APPEND abs_includes ${inc})
  endforeach()
  if(ARG_GROUP)
    set(module argus_${ARG_GROUP}_${ARG_NAME})
    set(alias argus::${ARG_GROUP}::${ARG_NAME})
  else()
    set(module argus_${ARG_NAME})
    set(alias argus::${ARG_NAME})
  endif()
  if(ARG_INTERFACE)
    add_library(${module} INTERFACE)
    if(abs_sources)
      target_sources(${module} INTERFACE ${abs_sources})
    endif()
    target_include_directories(${module} INTERFACE ${abs_includes})
    if(ARG_DEPENDS OR ARG_SYSTEM_DEPENDS)
      target_link_libraries(${module} INTERFACE ${ARG_DEPENDS}
                                                 ${ARG_SYSTEM_DEPENDS})
    endif()
  else()
    add_library(${module} STATIC ${abs_sources})
    target_include_directories(${module} PUBLIC ${abs_includes})
    if(ARG_DEPENDS OR ARG_SYSTEM_DEPENDS)
      target_link_libraries(${module} PUBLIC ${ARG_DEPENDS}
                                             ${ARG_SYSTEM_DEPENDS})
    endif()
  endif()
  add_library(${alias} ALIAS ${module})
endfunction()

function(argus_lib)
  cmake_parse_arguments(ARG "HEADER_ONLY" "NAME"
                        "SOURCES;INCLUDES;DEPENDS;SYSTEM_DEPENDS" ${ARGN})
  argus_reject_unknown_args(argus_lib)
  if(NOT ARG_NAME)
    message(FATAL_ERROR "argus_lib requires NAME")
  endif()
  set(kind "")
  if(ARG_HEADER_ONLY)
    list(APPEND kind INTERFACE)
  endif()
  argus_grouped_module(NAME ${ARG_NAME} GROUP lib ${kind}
      SOURCES ${ARG_SOURCES} INCLUDES ${ARG_INCLUDES}
      DEPENDS ${ARG_DEPENDS} SYSTEM_DEPENDS ${ARG_SYSTEM_DEPENDS})
endfunction()

function(argus_contracts)
  cmake_parse_arguments(ARG "" "NAME"
                        "SOURCES;INCLUDES;DEPENDS;SYSTEM_DEPENDS" ${ARGN})
  argus_reject_unknown_args(argus_contracts)
  if(NOT ARG_NAME)
    message(FATAL_ERROR "argus_contracts requires NAME")
  endif()
  argus_grouped_module(NAME ${ARG_NAME} GROUP contracts INTERFACE
      SOURCES ${ARG_SOURCES} INCLUDES ${ARG_INCLUDES}
      DEPENDS ${ARG_DEPENDS} SYSTEM_DEPENDS ${ARG_SYSTEM_DEPENDS})
endfunction()

function(argus_module)
  cmake_parse_arguments(ARG "HEADER_ONLY" "NAME"
                        "SOURCES;INCLUDES;DEPENDS;SYSTEM_DEPENDS" ${ARGN})
  argus_reject_unknown_args(argus_module)
  set(kind "")
  if(ARG_HEADER_ONLY)
    list(APPEND kind INTERFACE)
  endif()
  argus_grouped_module(NAME ${ARG_NAME} GROUP "" ${kind}
      SOURCES ${ARG_SOURCES} INCLUDES ${ARG_INCLUDES}
      DEPENDS ${ARG_DEPENDS} SYSTEM_DEPENDS ${ARG_SYSTEM_DEPENDS})
endfunction()

function(argus_grpc_absl_bridge)
  if(TARGET argus_client_grpc_bridge_entry)
    return()
  endif()
  argus_contracts_substrate()
  get_property(second_flavor GLOBAL PROPERTY ARGUS_SECOND_ABSEIL_FLAVOR)
  if(NOT second_flavor)
    return()
  endif()
  set(bridge_dir ${ARGUS_CMAKE_DIR}/../packages/lib/grpc/src/grpc)
  foreach(side entry exit)
    add_library(argus_client_grpc_bridge_${side} OBJECT
                ${bridge_dir}/grpc-cq-bridge-${side}.cc)
    set_target_properties(argus_client_grpc_bridge_${side} PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_options(argus_client_grpc_bridge_${side} PRIVATE -Wall -Wextra)
  endforeach()
  get_property(protobuf_includes GLOBAL PROPERTY ARGUS_PROTOBUF_INCLUDES)
  target_include_directories(argus_client_grpc_bridge_entry SYSTEM PRIVATE
                             ${protobuf_includes}
                             ${ARGUS_CMAKE_DIR}/../packages/lib/grpc/src)
  get_property(bridge_protobuf GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET)
  get_property(bridge_grpc GLOBAL PROPERTY ARGUS_GRPC_TARGET)
  target_link_libraries(argus_client_grpc_bridge_entry PRIVATE
                        ${bridge_protobuf} ${bridge_grpc})
  target_link_libraries(argus_client_grpc_bridge_exit PRIVATE ${bridge_grpc})
endfunction()

function(argus_grpc_client_base)
  if(TARGET argus_client_grpc_base)
    return()
  endif()
  argus_contracts_substrate()
  set(base_dir ${ARGUS_CMAKE_DIR}/../packages/lib/grpc/src/grpc)
  add_library(argus_client_grpc_base OBJECT ${base_dir}/grpc-client-base.cc
                                            ${base_dir}/grpc-server-drain.cc
                                            ${base_dir}/fleet-caller-gate.cc)
  set_target_properties(argus_client_grpc_base PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  target_include_directories(argus_client_grpc_base PUBLIC
                             ${ARGUS_CMAKE_DIR}/../packages/lib/grpc/src)
  get_property(base_grpc GLOBAL PROPERTY ARGUS_GRPC_TARGET)
  target_link_libraries(argus_client_grpc_base PUBLIC ${base_grpc})
  target_compile_options(argus_client_grpc_base PRIVATE -Wall -Wextra)
endfunction()

function(argus_contracts_substrate)
  get_property(substrate_done GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET)
  if(substrate_done)
    return()
  endif()
  find_package(Threads REQUIRED)
  if(ARGUS_SYSTEM_PROTOBUF)
    find_program(system_pkgcfg pkg-config REQUIRED)
    execute_process(
      COMMAND ${system_pkgcfg} --libs grpc++
      OUTPUT_VARIABLE system_grpc_libs RESULT_VARIABLE system_grpc_result
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT system_grpc_result EQUAL 0)
      message(FATAL_ERROR
              "argus_contracts_substrate: pkg-config cannot resolve grpc++")
    endif()
    separate_arguments(system_grpc_items NATIVE_COMMAND "${system_grpc_libs}")
    set(system_grpc_resolved "")
    foreach(item IN LISTS system_grpc_items)
      if(item MATCHES "^-l(.+)$")
        set(system_lib_var "system_lib_${CMAKE_MATCH_1}")
        find_library(${system_lib_var} ${CMAKE_MATCH_1}
                     PATHS "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
                     NO_DEFAULT_PATH)
        if(${system_lib_var})
          list(APPEND system_grpc_resolved "${${system_lib_var}}")
        else()
          list(APPEND system_grpc_resolved "${item}")
        endif()
      else()
        list(APPEND system_grpc_resolved "${item}")
      endif()
    endforeach()
    set(system_grpc_items "${system_grpc_resolved}")
    find_program(system_grpc_plugin grpc_cpp_plugin REQUIRED)
    find_library(system_grpcpp_lib grpc++
                 PATHS "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
                 NO_DEFAULT_PATH REQUIRED)
    find_library(system_protobuf_lib protobuf
                 PATHS "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
                 NO_DEFAULT_PATH REQUIRED)
    find_program(system_protoc protoc REQUIRED)
    add_executable(gRPC::grpc_cpp_plugin IMPORTED)
    set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES
                          IMPORTED_LOCATION "${system_grpc_plugin}")
    add_library(argus_system_grpc++ UNKNOWN IMPORTED)
    set_target_properties(argus_system_grpc++ PROPERTIES
        IMPORTED_LOCATION "${system_grpcpp_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "/usr/include"
        INTERFACE_LINK_LIBRARIES "${system_grpc_items}")
    add_library(argus_system_protobuf UNKNOWN IMPORTED)
    set_target_properties(argus_system_protobuf PROPERTIES
        IMPORTED_LOCATION "${system_protobuf_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "/usr/include")
    set_property(GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET
                 "argus_system_protobuf")
    set_property(GLOBAL PROPERTY ARGUS_GRPC_TARGET "argus_system_grpc++")
    set_property(GLOBAL APPEND PROPERTY
                 ARGUS_PROTOBUF_INCLUDES "/usr/include")
    set_property(GLOBAL PROPERTY ARGUS_PROTOC "${system_protoc}")
  else()
    find_package(Protobuf QUIET)
    if(NOT Protobuf_FOUND)
      message(FATAL_ERROR
              "argus_contracts_substrate: no protobuf substrate found")
    endif()

    set(protobuf_pkg "")
    foreach(cfg _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL "")
      if(NOT protobuf_pkg AND DEFINED protobuf_PACKAGE_FOLDER${cfg})
        set(protobuf_pkg "${protobuf_PACKAGE_FOLDER${cfg}}")
      endif()
    endforeach()
    if(protobuf_pkg)
      if(EXISTS "${protobuf_pkg}/include/google/protobuf/message.h")
        set_property(GLOBAL APPEND PROPERTY
                     ARGUS_PROTOBUF_INCLUDES "${protobuf_pkg}/include")
      endif()
      get_property(protoc_recorded GLOBAL PROPERTY ARGUS_PROTOC)
      if(NOT protoc_recorded AND EXISTS "${protobuf_pkg}/bin/protoc")
        set_property(GLOBAL PROPERTY ARGUS_PROTOC "${protobuf_pkg}/bin/protoc")
      endif()
    endif()
  endif()

  if(NOT ARGUS_SYSTEM_PROTOBUF)
    find_package(gRPC CONFIG QUIET)
    if(NOT gRPC_FOUND)
      list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local/argus-thirdparty/grpc")
      find_package(gRPC CONFIG REQUIRED)
    endif()
    set_property(GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET
                 "protobuf::libprotobuf")
    set_property(GLOBAL PROPERTY ARGUS_GRPC_TARGET "gRPC::grpc++")
  endif()
  if(NOT ARGUS_SYSTEM_PROTOBUF AND gRPC_DIR MATCHES "argus-thirdparty")
    get_target_property(grpc_location gRPC::grpc++ IMPORTED_LOCATION)
    if(grpc_location)
      cmake_path(GET grpc_location PARENT_PATH grpc_lib_dir)
      set_property(GLOBAL APPEND PROPERTY ARGUS_RPATH_DIRS "${grpc_lib_dir}")
    endif()
    set(ARGUS_VENDORED_GRPC TRUE)
  endif()

  set(abseil_pkg "")
  foreach(cfg _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL "")
    if(NOT abseil_pkg AND NOT ARGUS_SYSTEM_PROTOBUF
       AND DEFINED abseil_PACKAGE_FOLDER${cfg})
      set(abseil_pkg "${abseil_PACKAGE_FOLDER${cfg}}")
    endif()
  endforeach()
  if(abseil_pkg AND EXISTS "${abseil_pkg}/include/absl/strings/str_cat.h")
    set_property(GLOBAL APPEND PROPERTY
                 ARGUS_PROTOBUF_INCLUDES "${abseil_pkg}/include")
    set_property(GLOBAL PROPERTY ARGUS_SECOND_ABSEIL_FLAVOR TRUE)
  endif()

  if(ARGUS_VENDORED_GRPC)
    argus_grpc_absl_bridge()
  endif()
endfunction()

function(argus_runtime_rpath target)
  get_property(rpath_dirs GLOBAL PROPERTY ARGUS_RPATH_DIRS)
  if(NOT rpath_dirs)
    return()
  endif()
  get_target_property(existing_rpath ${target} BUILD_RPATH)
  set(new_rpath "${existing_rpath}")
  foreach(dir IN LISTS rpath_dirs)
    list(APPEND new_rpath "${dir}")
  endforeach()
  set_target_properties(${target} PROPERTIES
      BUILD_RPATH "${new_rpath}"
      INSTALL_RPATH "${new_rpath}")
endfunction()

function(argus_client_module)
  cmake_parse_arguments(ARG "" "NAME;PROTO_ROOT;GROUP"
                        "PROTO;SOURCES;INCLUDES;DEPENDS" ${ARGN})
  argus_reject_unknown_args(argus_client_module)
  if(NOT ARG_NAME OR NOT ARG_PROTO)
    message(FATAL_ERROR "argus_client_module requires NAME and PROTO")
  endif()
  if(NOT ARG_GROUP)
    set(ARG_GROUP clients)
  endif()
  set(module argus_${ARG_GROUP}_${ARG_NAME})
  set(module_alias argus::${ARG_GROUP}::${ARG_NAME})
  argus_contracts_substrate()
  find_package(Threads REQUIRED)
  find_package(gRPC CONFIG QUIET)
  if(NOT gRPC_FOUND)
    list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local/argus-thirdparty/grpc")
    find_package(gRPC CONFIG REQUIRED)
  endif()

  get_property(protoc_bin GLOBAL PROPERTY ARGUS_PROTOC)
  get_property(grpc_target GLOBAL PROPERTY ARGUS_GRPC_TARGET)
  if(NOT protoc_bin)
    find_program(protoc_bin NAMES protoc REQUIRED)
    set_property(GLOBAL PROPERTY ARGUS_PROTOC "${protoc_bin}")
  endif()

  set(root ${ARG_PROTO_ROOT})
  if(NOT root)
    set(root proto)
  endif()
  cmake_path(ABSOLUTE_PATH root BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
             NORMALIZE)
  set(gen_root ${CMAKE_CURRENT_BINARY_DIR}/argus-${ARG_GROUP}-${ARG_NAME}/generated)

  set(gen_sources "")
  foreach(proto IN LISTS ARG_PROTO)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${proto})
      cmake_path(ABSOLUTE_PATH proto BASE_DIRECTORY
                 ${CMAKE_CURRENT_SOURCE_DIR} NORMALIZE)
    else()
      cmake_path(ABSOLUTE_PATH proto BASE_DIRECTORY ${root} NORMALIZE)
    endif()
    file(RELATIVE_PATH rel ${root} ${proto})
    cmake_path(GET rel PARENT_PATH proto_dir)
    cmake_path(GET rel STEM proto_name)
    set(gen_dir ${gen_root}/${proto_dir})
    set(gen_files
        ${gen_dir}/${proto_name}.pb.cc
        ${gen_dir}/${proto_name}.pb.h
        ${gen_dir}/${proto_name}.grpc.pb.cc
        ${gen_dir}/${proto_name}.grpc.pb.h)
    add_custom_command(OUTPUT ${gen_files}
        COMMAND ${CMAKE_COMMAND} -E env
                "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:${grpc_target}>:$ENV{LD_LIBRARY_PATH}"
                ${protoc_bin}
        ARGS --proto_path=${root}
             --cpp_out=${gen_root}
             --grpc_out=${gen_root}
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             ${proto}
        DEPENDS ${proto} gRPC::grpc_cpp_plugin
        COMMENT "Generating gRPC stubs for ${ARG_NAME}"
        VERBATIM)
    list(APPEND gen_sources ${gen_files})
  endforeach()

  set(client_sources "")
  foreach(src IN LISTS ARG_SOURCES)
    cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
               NORMALIZE)
    list(APPEND client_sources ${src})
  endforeach()
  set(abs_includes "")
  foreach(inc IN LISTS ARG_INCLUDES)
    cmake_path(ABSOLUTE_PATH inc BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
               NORMALIZE)
    list(APPEND abs_includes ${inc})
  endforeach()

  add_library(${module} STATIC ${gen_sources} ${client_sources})
  add_library(${module_alias} ALIAS ${module})
  get_property(protobuf_includes GLOBAL PROPERTY ARGUS_PROTOBUF_INCLUDES)
  if(protobuf_includes)
    target_include_directories(${module} SYSTEM PUBLIC
                               ${protobuf_includes})
  endif()
  target_include_directories(${module} SYSTEM PUBLIC ${gen_root})
  if(abs_includes)
    target_include_directories(${module} PUBLIC ${abs_includes})
  endif()
  argus_grpc_client_base()
  set(bridge argus_client_grpc_base)
  if(TARGET argus_client_grpc_bridge_entry)
    list(APPEND bridge argus_client_grpc_bridge_entry argus_client_grpc_bridge_exit)
  endif()
  get_property(protobuf_target GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET)
  get_property(grpc_target GLOBAL PROPERTY ARGUS_GRPC_TARGET)
  target_link_libraries(${module}
      PUBLIC ${bridge} ${protobuf_target} ${grpc_target} ${ARG_DEPENDS})
endfunction()

function(argus_clients)
  cmake_parse_arguments(ARG "" "NAME;PROTO_ROOT"
                        "PROTO;SOURCES;INCLUDES;DEPENDS;SYSTEM_DEPENDS" ${ARGN})
  argus_reject_unknown_args(argus_clients)
  if(NOT ARG_NAME)
    message(FATAL_ERROR "argus_clients requires NAME")
  endif()
  if(ARG_PROTO)
    argus_client_module(NAME ${ARG_NAME} GROUP clients
        PROTO_ROOT ${ARG_PROTO_ROOT} PROTO ${ARG_PROTO}
        SOURCES ${ARG_SOURCES} INCLUDES ${ARG_INCLUDES}
        DEPENDS ${ARG_DEPENDS} ${ARG_SYSTEM_DEPENDS})
  else()
    argus_grouped_module(NAME ${ARG_NAME} GROUP clients
        SOURCES ${ARG_SOURCES} INCLUDES ${ARG_INCLUDES}
        DEPENDS ${ARG_DEPENDS} SYSTEM_DEPENDS ${ARG_SYSTEM_DEPENDS})
  endif()
endfunction()

set(ARGUS_DOCTEST_SKIPPED_PATTERN "\\| *0 failed \\| *[1-9][0-9]* skipped")

function(argus_count_skipped_tests directory)
  get_property(tests DIRECTORY "${directory}" PROPERTY TESTS)
  foreach(name IN LISTS tests)
    get_property(code TEST "${name}" DIRECTORY "${directory}" PROPERTY SKIP_RETURN_CODE)
    get_property(pattern TEST "${name}" DIRECTORY "${directory}" PROPERTY SKIP_REGULAR_EXPRESSION)
    if(NOT code AND NOT pattern)
      set_property(TEST "${name}" DIRECTORY "${directory}"
          PROPERTY SKIP_REGULAR_EXPRESSION "${ARGUS_DOCTEST_SKIPPED_PATTERN}")
    endif()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    argus_count_skipped_tests("${child}")
  endforeach()
endfunction()

function(argus_queue_skipped_test_count)
  get_property(queued GLOBAL PROPERTY ARGUS_SKIPPED_TESTS_QUEUED)
  if(NOT queued)
    set_property(GLOBAL PROPERTY ARGUS_SKIPPED_TESTS_QUEUED TRUE)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL argus_count_skipped_tests "${CMAKE_SOURCE_DIR}")
  endif()
endfunction()

function(argus_service)
  cmake_parse_arguments(ARG "" "NAME;MAIN" "MODULES;DEPENDS;PORTS" ${ARGN})
  argus_reject_unknown_args(argus_service)
  if(NOT ARG_NAME OR NOT ARG_MAIN)
    message(FATAL_ERROR "argus_service requires NAME and MAIN")
  endif()
  add_executable(${ARG_NAME} ${ARG_MAIN})
  target_link_libraries(${ARG_NAME} PRIVATE ${ARG_MODULES} ${ARG_DEPENDS})
  target_compile_options(${ARG_NAME} PRIVATE -Wall -Wextra)
  set_target_properties(${ARG_NAME} PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN")
  argus_runtime_rpath(${ARG_NAME})
  if(ARG_PORTS)
    set_target_properties(${ARG_NAME} PROPERTIES ARGUS_PORTS "${ARG_PORTS}")
  endif()
  argus_queue_skipped_test_count()
endfunction()
