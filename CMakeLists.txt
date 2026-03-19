cmake_minimum_required(VERSION 3.20)

project(corouv
  VERSION 0.1.0
  LANGUAGES C CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

file(GLOB_RECURSE BEARSSL_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bearssl/src/*.c
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------
# Dependencies (sources)
# -----------------------

# libuv (build from source)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libuv EXCLUDE_FROM_ALL)

# async_simple is effectively header-only, but its CMakeLists is quite invasive
# (global flags, -Werror, tests). We wire it as an INTERFACE library ourselves.
find_package(Threads REQUIRED)
add_library(async_simple INTERFACE)
target_include_directories(async_simple SYSTEM INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/async_simple>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(async_simple INTERFACE Threads::Threads)

# -----------------------
# corouv library
# -----------------------
add_library(corouv
  src/http.cpp
  src/https.cpp
  src/multipart.cpp
  src/net.cpp
  src/sql.cpp
  src/transport.cpp
  src/uv_executor.cpp
  ${BEARSSL_SOURCES}
  third_party/picohttpparser/picohttpparser.c
)
add_library(corouv::corouv ALIAS corouv)

target_include_directories(corouv PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(corouv PUBLIC
  uv_a
  async_simple
  ${CMAKE_DL_LIBS}
)

target_include_directories(corouv PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bearssl/inc
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bearssl/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/picohttpparser
)

target_compile_features(corouv PUBLIC cxx_std_20)

if (MSVC)
  target_compile_options(corouv PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
  )
else()
  target_compile_options(corouv PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )
endif()

# -----------------------
# Install / package
# -----------------------
install(TARGETS corouv async_simple uv_a
  EXPORT corouvTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "*.hpp"
  PATTERN "*.ipp"
  PATTERN "*.inc"
)

install(DIRECTORY third_party/libuv/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY third_party/async_simple/async_simple/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/async_simple
  FILES_MATCHING
  PATTERN "*.h"
  PATTERN "*.hpp"
  PATTERN "*.ipp"
  PATTERN "*.inc"
  PATTERN "*.cppm"
)

install(EXPORT corouvTargets
  FILE corouvTargets.cmake
  NAMESPACE corouv::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corouv
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/corouvConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/corouvConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corouv
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/corouvConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/corouvConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/corouvConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corouv
)
