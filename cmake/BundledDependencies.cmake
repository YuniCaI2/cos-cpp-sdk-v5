include_guard(GLOBAL)

# Build BoringSSL / libcurl / GTest from git submodules under third_party/.

set(_cos_tp "${PROJECT_SOURCE_DIR}/third_party")
set(_cos_required_deps boringssl curl)
if(BUILD_UNITTEST)
  list(APPEND _cos_required_deps googletest)
endif()
foreach(_dep IN LISTS _cos_required_deps)
  if(NOT EXISTS "${_cos_tp}/${_dep}/CMakeLists.txt")
    message(FATAL_ERROR
      "Missing git submodule '${_dep}'.\n"
      "Run: git submodule update --init --recursive")
  endif()
endforeach()

set(_cos_prev_build_shared_libs "${BUILD_SHARED_LIBS}")
set(_cos_prev_install_prefix "${CMAKE_INSTALL_PREFIX}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

# --- BoringSSL ---
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPENSSL_NO_ASM ON CACHE BOOL "" FORCE)
set(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT FALSE)
add_subdirectory("${_cos_tp}/boringssl" "${CMAKE_BINARY_DIR}/_deps/boringssl"
  EXCLUDE_FROM_ALL)

if(NOT TARGET OpenSSL::Crypto)
  if(TARGET crypto)
    add_library(OpenSSL::Crypto ALIAS crypto)
  else()
    message(FATAL_ERROR "BoringSSL crypto target not found")
  endif()
endif()
if(NOT TARGET OpenSSL::SSL)
  if(TARGET ssl)
    add_library(OpenSSL::SSL ALIAS ssl)
  else()
    message(FATAL_ERROR "BoringSSL ssl target not found")
  endif()
endif()

# --- libcurl (Windows uses Schannel for TLS) ---
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_EXPORT_TARGET ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${_cos_tp}/curl" "${CMAKE_BINARY_DIR}/_deps/curl"
  EXCLUDE_FROM_ALL)

if(NOT TARGET CURL::libcurl)
  if(TARGET libcurl)
    add_library(CURL::libcurl ALIAS libcurl)
  elseif(TARGET libcurl_static)
    add_library(CURL::libcurl ALIAS libcurl_static)
  else()
    message(FATAL_ERROR "libcurl target not found")
  endif()
endif()

if(BUILD_UNITTEST)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  add_subdirectory("${_cos_tp}/googletest" "${CMAKE_BINARY_DIR}/_deps/googletest"
    EXCLUDE_FROM_ALL)
endif()

if(_cos_prev_build_shared_libs STREQUAL "")
  unset(BUILD_SHARED_LIBS CACHE)
else()
  set(BUILD_SHARED_LIBS "${_cos_prev_build_shared_libs}" CACHE BOOL
    "Build shared libraries" FORCE)
endif()
set(CMAKE_INSTALL_PREFIX "${_cos_prev_install_prefix}" CACHE PATH
  "Install path prefix" FORCE)
