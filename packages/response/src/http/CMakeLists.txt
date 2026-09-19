# Transitional alias: ApiResponse compiles in pkg::response itself, so this
# stays only so existing consumers keep linking pkg::response-http untouched.
add_library(response-http INTERFACE)
add_library(pkg::response-http ALIAS response-http)
target_link_libraries(response-http INTERFACE pkg::response)
