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
  # fastText's CMakeLists still requires CMake <3.5 compatibility.
  set(ARGUS_FASTTEXT_POLICY ${CMAKE_POLICY_VERSION_MINIMUM})
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  add_subdirectory(fastText EXCLUDE_FROM_ALL)
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
  endif()
else()
  message(WARNING "Argus: fastText submodule not found at third_party/fastText")
endif()

# ---------------------------------------------------------------------------
# InspireFace (face recognition SDK)
# ---------------------------------------------------------------------------
# NOTE: InspireFace is heavy and pulls its own deps (MNN, InspireCV) which are
# NOT git submodules -- they must be cloned manually into
# third_party/inspireface/3rdparty/{MNN,InspireCV} (see that repo's README).
# It is therefore opt-in via ARGUS_BUILD_INSPIREFACE to keep the default build
# green and fast. Enable it (and provision deps + models) when needed.
option(ARGUS_BUILD_INSPIREFACE "Build the InspireFace face-recognition SDK" OFF)

if(ARGUS_BUILD_INSPIREFACE)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/inspireface/CMakeLists.txt")
    message(STATUS "Argus: integrating InspireFace submodule")
    set(ISF_BUILD_WITH_TEST OFF CACHE BOOL "" FORCE)
    set(ISF_BUILD_WITH_SAMPLE OFF CACHE BOOL "" FORCE)
    set(ISF_NEVER_USE_OPENCV ON CACHE BOOL "" FORCE)
    set(ISF_ENABLE_RKNN OFF CACHE BOOL "" FORCE)
    set(ISF_ENABLE_RGA OFF CACHE BOOL "" FORCE)
    add_subdirectory(inspireface EXCLUDE_FROM_ALL)
    if(TARGET InspireFace)
      target_include_directories(InspireFace INTERFACE
          ${CMAKE_CURRENT_SOURCE_DIR}/inspireface/cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/inspireface/cpp/inspireface/include)
      message(STATUS "Argus: InspireFace target -> InspireFace")
    endif()
  else()
    message(WARNING "Argus: InspireFace submodule not found at third_party/inspireface")
  endif()
else()
  message(STATUS "Argus: InspireFace integration disabled (set ARGUS_BUILD_INSPIREFACE=ON to enable)")
endif()
