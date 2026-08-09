if(EXISTS "${SRC}")
  file(COPY_FILE "${SRC}" "${DST}" ONLY_IF_DIFFERENT)
endif()
