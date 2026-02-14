add_executable(basic_usage basic_usage.cpp)
target_link_libraries(basic_usage PRIVATE ms-runloop pthread)

add_executable(event_notifier event_notifier.cpp)
target_link_libraries(event_notifier PRIVATE ms-runloop pthread)
