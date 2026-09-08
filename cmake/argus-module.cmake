# AGENTS.md rule 25 build helpers: the folder IS the module; consumers link
# by name (argus::<name>, argus::sdk-<name>).

include_guard(GLOBAL)

# argus_module(NAME <name> [SOURCES ...] [INCLUDES ...] [DEPENDS ...]
#              [SYSTEM_DEPENDS ...]) -> static lib argus_<name> / argus::<name>
function(argus_module)
  cmake_parse_arguments(ARG "" "NAME" "SOURCES;INCLUDES;DEPENDS;SYSTEM_DEPENDS"
                        ${ARGN})
  if(NOT ARG_NAME)
    message(FATAL_ERROR "argus_module requires NAME")
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
  add_library(argus_${ARG_NAME} STATIC ${abs_sources})
  add_library(argus::${ARG_NAME} ALIAS argus_${ARG_NAME})
  target_include_directories(argus_${ARG_NAME} PUBLIC ${abs_includes})
  if(ARG_DEPENDS OR ARG_SYSTEM_DEPENDS)
    target_link_libraries(argus_${ARG_NAME} PUBLIC ${ARG_DEPENDS}
                                                   ${ARG_SYSTEM_DEPENDS})
  endif()
endfunction()

# ABI bridge over the vendored gRPC boundary (system vs Conan abseil inline
# namespaces); the full why lives in argus-contracts/CONTEXT.md.
function(argus_grpc_absl_bridge)
  if(TARGET argus_sdk_grpc_bridge_entry)
    return()
  endif()
  set(bridge_dir ${CMAKE_CURRENT_LIST_DIR}/../argus-contracts/sdk/grpc)
  foreach(side entry exit)
    add_library(argus_sdk_grpc_bridge_${side} OBJECT
                ${bridge_dir}/grpc-cq-bridge-${side}.cc)
    set_target_properties(argus_sdk_grpc_bridge_${side} PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_compile_options(argus_sdk_grpc_bridge_${side} PRIVATE -Wall -Wextra)
  endforeach()
  get_property(protobuf_includes GLOBAL PROPERTY ARGUS_PROTOBUF_INCLUDES)
  target_include_directories(argus_sdk_grpc_bridge_entry SYSTEM PRIVATE
                             ${protobuf_includes} ${bridge_dir})
  target_link_libraries(argus_sdk_grpc_bridge_entry PRIVATE
                        protobuf::libprotobuf gRPC::grpc++)
  target_link_libraries(argus_sdk_grpc_bridge_exit PRIVATE gRPC::grpc++)
endfunction()

# The shared client base every SDK wrapper builds on (channel credentials,
# deadlines, x-argus-* caller metadata) — declared once, linked into each
# argus_sdk_module the same way the ABI bridge is.
function(argus_grpc_client_base)
  if(TARGET argus_sdk_grpc_base)
    return()
  endif()
  set(base_dir ${CMAKE_CURRENT_LIST_DIR}/../argus-contracts/sdk/grpc)
  add_library(argus_sdk_grpc_base OBJECT ${base_dir}/grpc-client-base.cc)
  set_target_properties(argus_sdk_grpc_base PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  target_include_directories(argus_sdk_grpc_base PUBLIC ${base_dir})
  target_link_libraries(argus_sdk_grpc_base PUBLIC gRPC::grpc++)
  target_compile_options(argus_sdk_grpc_base PRIVATE -Wall -Wextra)
endfunction()

# ARGUS_SYSTEM_PROTOBUF swaps the SDK's protobuf/gRPC for the Debian stack.
function(argus_contracts_substrate)
  get_property(substrate_done GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET)
  if(substrate_done)
    return()
  endif()
  find_package(Threads REQUIRED)
  if(ARGUS_SYSTEM_PROTOBUF)
    # The Debian stack is wired by hand from pkg-config (conan shadows the
    # find_package names); see argus-contracts/CONTEXT.md.
    find_program(system_pkgcfg pkg-config REQUIRED)
    execute_process(
      COMMAND ${system_pkgcfg} --libs grpc++
      OUTPUT_VARIABLE system_grpc_libs RESULT_VARIABLE system_grpc_result
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT system_grpc_result EQUAL 0)
      message(FATAL_ERROR "argus-contracts: pkg-config cannot resolve grpc++")
    endif()
    separate_arguments(system_grpc_items NATIVE_COMMAND "${system_grpc_libs}")
    # -l items must resolve inside Debian: a -L path from the conan stack
    # (its own abseil) would shadow -labsl_* and break the debian5 symbols.
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
      message(FATAL_ERROR "argus-contracts: no protobuf substrate found")
    endif()

    # The conan module-mode export carries no include dirs; derive them from
    # the package folder so the SDK binds to the graph's protobuf runtime.
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
  # The prefix persists in the cache, so test gRPC_DIR not this run's result.
  if(NOT ARGUS_SYSTEM_PROTOBUF AND gRPC_DIR MATCHES "argus-thirdparty")
    get_target_property(grpc_location gRPC::grpc++ IMPORTED_LOCATION)
    if(grpc_location)
      cmake_path(GET grpc_location PARENT_PATH grpc_lib_dir)
      set_property(GLOBAL APPEND PROPERTY ARGUS_RPATH_DIRS "${grpc_lib_dir}")
    endif()
    set(ARGUS_VENDORED_GRPC TRUE)
  endif()

  # The conan abseil export carries no include dirs either.
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
  endif()

  if(ARGUS_VENDORED_GRPC)
    argus_grpc_absl_bridge()
  endif()
endfunction()

# Appends the recorded substrate lib dirs to a runtime target's rpath.
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

# argus_sdk_module(NAME <name> [PROTO_ROOT <dir>] PROTO <path>... [SOURCES ...]
#                  [INCLUDES ...] [DEPENDS ...]) -> argus::sdk-<name>; stubs
# generate into the build tree, protobuf types stay behind the SDK headers.
function(argus_sdk_module)
  cmake_parse_arguments(ARG "" "NAME;PROTO_ROOT"
                        "PROTO;SOURCES;INCLUDES;DEPENDS" ${ARGN})
  if(NOT ARG_NAME OR NOT ARG_PROTO)
    message(FATAL_ERROR "argus_sdk_module requires NAME and PROTO")
  endif()
  argus_contracts_substrate()

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
  set(gen_root ${CMAKE_CURRENT_BINARY_DIR}/argus-sdk-${ARG_NAME}/generated)

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

  set(sdk_sources "")
  foreach(src IN LISTS ARG_SOURCES)
    cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
               NORMALIZE)
    list(APPEND sdk_sources ${src})
  endforeach()
  set(abs_includes "")
  foreach(inc IN LISTS ARG_INCLUDES)
    cmake_path(ABSOLUTE_PATH inc BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
               NORMALIZE)
    list(APPEND abs_includes ${inc})
  endforeach()

  add_library(argus_sdk_${ARG_NAME} STATIC ${gen_sources} ${sdk_sources})
  add_library(argus::sdk-${ARG_NAME} ALIAS argus_sdk_${ARG_NAME})
  get_property(protobuf_includes GLOBAL PROPERTY ARGUS_PROTOBUF_INCLUDES)
  if(protobuf_includes)
    target_include_directories(argus_sdk_${ARG_NAME} SYSTEM PUBLIC
                               ${protobuf_includes})
  endif()
  target_include_directories(argus_sdk_${ARG_NAME} SYSTEM PUBLIC ${gen_root})
  if(abs_includes)
    target_include_directories(argus_sdk_${ARG_NAME} PUBLIC ${abs_includes})
  endif()
  argus_grpc_client_base()
  set(bridge argus_sdk_grpc_base)
  if(TARGET argus_sdk_grpc_bridge_entry)
    list(APPEND bridge argus_sdk_grpc_bridge_entry argus_sdk_grpc_bridge_exit)
  endif()
  get_property(protobuf_target GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET)
  get_property(grpc_target GLOBAL PROPERTY ARGUS_GRPC_TARGET)
  target_link_libraries(argus_sdk_${ARG_NAME}
      PUBLIC ${bridge} ${protobuf_target} ${grpc_target} ${ARG_DEPENDS})
endfunction()

# argus_service(NAME <name> MAIN <main.cc> [MODULES ...] [DEPENDS ...]
#               [PORTS ...]) -> executable + module links + warning gate;
# PORTS are recorded as a target property (config owns runtime ports).
function(argus_service)
  cmake_parse_arguments(ARG "" "NAME;MAIN" "MODULES;DEPENDS;PORTS" ${ARGN})
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
endfunction()
