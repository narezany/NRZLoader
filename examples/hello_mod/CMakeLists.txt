add_library(hello_mod SHARED hello_mod.cpp)
target_link_libraries(hello_mod PRIVATE mcbe_mod_api)
set_target_properties(hello_mod PROPERTIES PREFIX "lib" OUTPUT_NAME "hello_mod")
