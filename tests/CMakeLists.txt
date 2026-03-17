cmake_minimum_required(VERSION 3.20)

project(corouv_tests
  VERSION 0.1.0
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
enable_testing()

add_subdirectory(.. ${CMAKE_CURRENT_BINARY_DIR}/corouv-core)

function(corouv_add_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE corouv::corouv)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

corouv_add_test(corouv_test_timer timer_test.cpp)
corouv_add_test(corouv_test_timeout timeout_test.cpp)
corouv_add_test(corouv_test_blocking blocking_test.cpp)
corouv_add_test(corouv_test_blocking_error blocking_error_test.cpp)
corouv_add_test(corouv_test_cancel cancel_test.cpp)
corouv_add_test(corouv_test_async_mutex async_mutex_test.cpp)
corouv_add_test(corouv_test_async_semaphore async_semaphore_test.cpp)
corouv_add_test(corouv_test_async_queue async_queue_test.cpp)
corouv_add_test(corouv_test_event event_test.cpp)
corouv_add_test(corouv_test_fs fs_test.cpp)
corouv_add_test(corouv_test_runtime runtime_test.cpp)
corouv_add_test(corouv_test_task_group task_group_test.cpp)
corouv_add_test(corouv_test_net net_test.cpp)
corouv_add_test(corouv_test_http http_test.cpp)

if (UNIX)
  corouv_add_test(corouv_test_poll poll_test.cpp)
  corouv_add_test(corouv_test_poll_overload poll_overload_test.cpp)
endif()
