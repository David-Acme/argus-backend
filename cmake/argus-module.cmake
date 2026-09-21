# AGENTS.md rule 25 build helpers: the folder IS the module; consumers link
# by name. A package names its group (argus::lib::<name>,
# argus::contracts::<domain>, argus::clients::<domain>); a service-local
# module stays argus::<name>.

include_guard(GLOBAL)

# This file's own directory, captured at include time. CMAKE_CURRENT_LIST_DIR
# inside a function resolves against the CALLER's list file, so paths to
# sibling folders must be anchored here instead.
set(ARGUS_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})

# A helper's keyword list is its contract, and cmake_parse_arguments does not
# enforce it. Two ways a misspelling survives, both measured on CMake 3.31:
#
#   * with no multi-value keyword open, the stray token lands in
#     ARG_UNPARSED_ARGUMENTS ('BOGUS;x' for `BOGUS x NAME n ...`);
#   * with one open, it is appended to THAT list, and ARG_UNPARSED_ARGUMENTS
#     stays empty -- `NAME n DEPENDS d SYSTEM_DEPEND Foo` parses to
#     ARG_DEPENDS='d;SYSTEM_DEPEND;Foo', which is the realistic typo and the
#     one that hurts: the token reaches target_link_libraries as a bare name
#     it cannot resolve, and the error surfaces at link time, far from the
#     call that made it. (cmake_parse_arguments' PARSE_ARGV form behaves the
#     same here.)
#
# So the check covers both: the unparsed list, and any element of a value list
# that is keyword-shaped. Keywords are ALL_CAPS by convention in this file and
# in CMake, and every legitimate value is a target, an alias, a path or a
# library name -- none of them all-caps -- so the shape test is precise
# against this tree rather than merely heuristic. It cannot catch a lowercase
# misspelling, which is indistinguishable from a library name.
#
# A MACRO, not a function: ARG_* are the calling helper's own variables, and a
# function would not see them.
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

# The shared body of the package helpers: the group is part of the target
# name, so a package cannot declare itself into the wrong tier (§2.5). STATIC
# unless INTERFACE, which is what a header-only package is. An empty GROUP is
# a module that names no tier -- a service-local module, or one of the
# packages §9.1 sends to "--" until the phase that moves it -- and keeps the
# bare argus_<name> / argus::<name>.
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

# argus_lib(NAME <name> [HEADER_ONLY] [SOURCES ...] [INCLUDES ...]
#           [DEPENDS ...] [SYSTEM_DEPENDS ...])
#           -> argus_lib_<name> / argus::lib::<name>; a header-only package
#           declares HEADER_ONLY and has no .cc.
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

# argus_contracts(NAME <domain> [SOURCES ...] [INCLUDES ...] [DEPENDS ...])
#           -> argus_contracts_<domain> / argus::contracts::<domain>, the
#           INTERFACE target a domain's C++ vocabulary crosses the wire in.
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

# argus_module(NAME <name> [HEADER_ONLY] [SOURCES ...] [INCLUDES ...]
#              [DEPENDS ...] [SYSTEM_DEPENDS ...])
#              -> argus_<name> / argus::<name> -- a module that names no group:
#              how a service spells its own feature modules, and how the seven
#              packages §9.1 sends to "--" are spelled until the phase that
#              moves each of them out of packages/.
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

# ABI bridge over the vendored gRPC boundary (system vs Conan abseil inline
# namespaces); the full why lives in packages/contracts/CONTEXT.md.
function(argus_grpc_absl_bridge)
  if(TARGET argus_client_grpc_bridge_entry)
    return()
  endif()
  argus_contracts_substrate()
  # The bridge joins two abseil flavors, and it only exists when there are two:
  # the entry defines the inline namespace the SDK's own TUs reference, the exit
  # calls the one inside libgrpc. With a single flavor both names are one
  # symbol, so the entry would interpose libgrpc's own implementation and the
  # pair would recurse on the first callback -- the hazard
  # packages/contracts/CONTEXT.md records for the standalone configure, which is
  # why a single-flavor tree leaves the bridge out entirely.
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
  # The package's include root (section 2.3), not the directory the bridge is
  # compiled from: the entry object spells its sibling
  # <grpc/grpc-cq-bridge.hxx>, the same spelling every consumer of this
  # package uses.
  target_include_directories(argus_client_grpc_bridge_entry SYSTEM PRIVATE
                             ${protobuf_includes}
                             ${ARGUS_CMAKE_DIR}/../packages/lib/grpc/src)
  get_property(bridge_protobuf GLOBAL PROPERTY ARGUS_PROTOBUF_TARGET)
  get_property(bridge_grpc GLOBAL PROPERTY ARGUS_GRPC_TARGET)
  target_link_libraries(argus_client_grpc_bridge_entry PRIVATE
                        ${bridge_protobuf} ${bridge_grpc})
  target_link_libraries(argus_client_grpc_bridge_exit PRIVATE ${bridge_grpc})
endfunction()

# The shared client base every client wrapper builds on (channel credentials,
# deadlines, x-argus-* caller metadata) — declared once, linked into each
# argus_client_module the same way the ABI bridge is.
function(argus_grpc_client_base)
  if(TARGET argus_client_grpc_base)
    return()
  endif()
  argus_contracts_substrate()
  set(base_dir ${ARGUS_CMAKE_DIR}/../packages/lib/grpc/src/grpc)
  add_library(argus_client_grpc_base OBJECT ${base_dir}/grpc-client-base.cc)
  set_target_properties(argus_client_grpc_base PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  # The package's include root (section 2.3): consumers spell
  # <grpc/grpc-client-base.hxx>, so the public dir is src/, not src/grpc/.
  target_include_directories(argus_client_grpc_base PUBLIC
                             ${ARGUS_CMAKE_DIR}/../packages/lib/grpc/src)
  get_property(base_grpc GLOBAL PROPERTY ARGUS_GRPC_TARGET)
  target_link_libraries(argus_client_grpc_base PUBLIC ${base_grpc})
  target_compile_options(argus_client_grpc_base PRIVATE -Wall -Wextra)
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
    # find_package names); see packages/contracts/CONTEXT.md.
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
      message(FATAL_ERROR
              "argus_contracts_substrate: no protobuf substrate found")
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
    # A second abseil flavor is in play: the SDK's TUs compile against the Conan
    # abseil inline namespace while the vendored gRPC carries the host's. That,
    # and only that, is when the bridge has two symbols to join (measured:
    # abseil 20260107 in the SDK against 20260526 inside libgrpc).
    set_property(GLOBAL PROPERTY ARGUS_SECOND_ABSEIL_FLAVOR TRUE)
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

# argus_client_module(NAME <name> GROUP <group> [PROTO_ROOT <dir>] PROTO <path>...
#                  [SOURCES ...] [INCLUDES ...] [DEPENDS ...])
#                  -> argus_<group>_<name> / argus::<group>::<name>; stubs
# generate into the build tree, protobuf types stay behind the module's headers.
# The three wire modules that are not a domain SDK pass the group they live in
# (packages/lib/grpc's health stubs, the response and tts wire contracts); every
# domain SDK goes through argus_clients, which fixes the group for it.
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
  # Imported gRPC targets are directory-scoped: re-resolve them in the
  # caller's scope so the stub codegen below sees them, and with them the
  # Threads::Threads that gRPCTargets names in their link interface — the
  # substrate early-returns once configured, so its own find_package no
  # longer runs for a second consumer directory. Cached after first.
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

# argus_clients(NAME <domain> [PROTO_ROOT <dir> PROTO <path>...] [SOURCES ...]
#               [INCLUDES ...] [DEPENDS ...] [SYSTEM_DEPENDS ...])
#               -> argus_clients_<domain> / argus::clients::<domain>. With
#               PROTO it is a gRPC SDK and generates its stubs; without one it
#               is a plain module over an HTTP wire, which is what the four
#               remote clients are.
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

# argus_service(NAME <name> MAIN <main.cc> [MODULES ...] [DEPENDS ...]
#               [PORTS ...]) -> executable + module links + warning gate;
# PORTS are recorded as a target property (config owns runtime ports).
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
endfunction()
