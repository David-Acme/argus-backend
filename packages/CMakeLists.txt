cmake_minimum_required(VERSION 3.25)
project(argus-packages VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

function(argus_packages_added root out_var)
  set(found "")
  get_property(children DIRECTORY "${root}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    list(APPEND found "${child}")
    argus_packages_added("${child}" nested)
    list(APPEND found ${nested})
  endforeach()
  set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

file(GLOB argus_package_lists CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*/*/CMakeLists.txt")
list(SORT argus_package_lists)
foreach(package_list IN LISTS argus_package_lists)
  cmake_path(GET package_list PARENT_PATH package_dir)
  argus_packages_added("${CMAKE_CURRENT_SOURCE_DIR}" added)
  if(NOT package_dir IN_LIST added)
    cmake_path(RELATIVE_PATH package_dir BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
               OUTPUT_VARIABLE package_relative)
    add_subdirectory("${package_dir}" "${CMAKE_BINARY_DIR}/packages/${package_relative}")
  endif()
endforeach()
