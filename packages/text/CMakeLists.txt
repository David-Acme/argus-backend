# text: text normalization shared by the memory and intent extractors.
# No dependencies beyond the standard library; consumers link argus::text.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_module(NAME text
    SOURCES
        src/shared/utils/text-norm/text-norm.cc
        src/shared/utils/text-norm/text-norm.hxx
    INCLUDES src)

target_compile_options(argus_text PRIVATE -Wall -Wextra)
