cmake_minimum_required(VERSION 3.20)

project(corouv_examples
  VERSION 0.1.0
  LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(COROUV_EXAMPLES_BUILD_LIBPQ "Build the optional libpq example when bundled example deps are available" ON)

# Reuse the core library directly from the parent source tree.
add_subdirectory(.. ${CMAKE_CURRENT_BINARY_DIR}/corouv-core)

add_executable(corouv_timer timer.cpp)
target_link_libraries(corouv_timer PRIVATE corouv::corouv)

add_executable(corouv_read_file read_file.cpp)
target_link_libraries(corouv_read_file PRIVATE corouv::corouv)

add_executable(corouv_blocking blocking.cpp)
target_link_libraries(corouv_blocking PRIVATE corouv::corouv)

add_executable(corouv_timeout timeout.cpp)
target_link_libraries(corouv_timeout PRIVATE corouv::corouv)

add_executable(corouv_udp_echo udp_echo.cpp)
target_link_libraries(corouv_udp_echo PRIVATE corouv::corouv)

add_executable(corouv_udp_options udp_options.cpp)
target_link_libraries(corouv_udp_options PRIVATE corouv::corouv)

add_executable(corouv_udp_channel udp_channel.cpp)
target_link_libraries(corouv_udp_channel PRIVATE corouv::corouv)

add_executable(corouv_udp_multicast udp_multicast.cpp)
target_link_libraries(corouv_udp_multicast PRIVATE corouv::corouv)

add_executable(corouv_event event.cpp)
target_link_libraries(corouv_event PRIVATE corouv::corouv)

add_executable(corouv_cancel cancel.cpp)
target_link_libraries(corouv_cancel PRIVATE corouv::corouv)

add_executable(corouv_task_group task_group.cpp)
target_link_libraries(corouv_task_group PRIVATE corouv::corouv)

add_executable(corouv_fs_write_read fs_write_read.cpp)
target_link_libraries(corouv_fs_write_read PRIVATE corouv::corouv)

add_executable(corouv_filesystem_ops filesystem_ops.cpp)
target_link_libraries(corouv_filesystem_ops PRIVATE corouv::corouv)

add_executable(corouv_filesystem_watch filesystem_watch.cpp)
target_link_libraries(corouv_filesystem_watch PRIVATE corouv::corouv)

add_executable(corouv_primitives concurrency_primitives.cpp)
target_link_libraries(corouv_primitives PRIVATE corouv::corouv)

add_executable(corouv_http_server http_server.cpp)
target_link_libraries(corouv_http_server PRIVATE corouv::corouv)

add_executable(corouv_http_client http_client.cpp)
target_link_libraries(corouv_http_client PRIVATE corouv::corouv)

add_executable(corouv_http_form_multipart http_form_and_multipart.cpp)
target_link_libraries(corouv_http_form_multipart PRIVATE corouv::corouv)

add_executable(corouv_web_builder web_builder.cpp)
target_link_libraries(corouv_web_builder PRIVATE corouv::corouv)

add_executable(corouv_sql_basic sql_basic.cpp)
target_link_libraries(corouv_sql_basic PRIVATE corouv::corouv)

set(COROUV_LIBPQ_EXAMPLE_INCLUDE_DIR
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/postgresql-install/include
)
set(COROUV_LIBPQ_EXAMPLE_SOURCE_INCLUDE_DIR
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/postgresql/src/include
)
set(COROUV_LIBPQ_EXAMPLE_LIBRARY
  ${CMAKE_CURRENT_LIST_DIR}/../third_party/postgresql-install/lib/libpq.so
)

if (COROUV_EXAMPLES_BUILD_LIBPQ
    AND EXISTS "${COROUV_LIBPQ_EXAMPLE_INCLUDE_DIR}/libpq-fe.h"
    AND EXISTS "${COROUV_LIBPQ_EXAMPLE_LIBRARY}"
    AND EXISTS "${COROUV_LIBPQ_EXAMPLE_SOURCE_INCLUDE_DIR}/postgres_ext.h")
  add_custom_target(corouv_build_libpq_example
    COMMAND ${CMAKE_CURRENT_LIST_DIR}/../scripts/build_libpq.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMENT "Build bundled libpq for the libpq example using podman"
    VERBATIM
  )

  add_library(corouv_example_libpq SHARED IMPORTED GLOBAL)
  set_target_properties(corouv_example_libpq PROPERTIES
    IMPORTED_LOCATION "${COROUV_LIBPQ_EXAMPLE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${COROUV_LIBPQ_EXAMPLE_INCLUDE_DIR};${COROUV_LIBPQ_EXAMPLE_SOURCE_INCLUDE_DIR}"
  )

  add_executable(corouv_libpq libpq.cpp)
  target_link_libraries(corouv_libpq PRIVATE
    corouv::corouv
    corouv_example_libpq
  )
  target_include_directories(corouv_libpq PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    "${COROUV_LIBPQ_EXAMPLE_INCLUDE_DIR}"
    "${COROUV_LIBPQ_EXAMPLE_SOURCE_INCLUDE_DIR}"
  )
  set_target_properties(corouv_libpq PROPERTIES
    BUILD_RPATH "${CMAKE_CURRENT_LIST_DIR}/../third_party/postgresql-install/lib"
  )
endif()
