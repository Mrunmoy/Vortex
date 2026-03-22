enable_language(C)

add_executable(c_timer_loop timer_loop.c)
target_link_libraries(c_timer_loop PRIVATE vortex stdc++)
target_include_directories(c_timer_loop PRIVATE ${CMAKE_SOURCE_DIR}/inc)
