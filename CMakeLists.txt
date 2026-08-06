cmake_minimum_required(VERSION 3.21...4.3)

project(cos-cpp-sdk
  VERSION 5.5.22
  DESCRIPTION "Tencent Cloud COS C++ SDK"
  LANGUAGES C CXX
)

include(GNUInstallDirs)

option(BUILD_UNITTEST "Build the optional unit and integration tests" OFF)
option(BUILD_DEMO "Build the optional demo programs" OFF)
option(BUILD_SHARED_LIB "Build a shared SDK in addition to the static SDK" OFF)
option(ENABLE_COVERAGE "Enable coverage instrumentation for the SDK" OFF)

# Kept so existing build scripts do not fail.  MD5 now uses the private EVP
# adapter unconditionally; the old Poco/OpenSSL switch no longer changes the
# public or private implementation graph.
option(USE_OPENSSL_MD5 "Deprecated compatibility option" OFF)
if(USE_OPENSSL_MD5)
  message(WARNING "USE_OPENSSL_MD5 is deprecated and has no effect; EVP is the SDK MD5 backend")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BundledDependencies.cmake")
find_package(Threads REQUIRED)

add_subdirectory(src)

if(BUILD_DEMO)
  add_subdirectory(demo)
endif()

if(BUILD_UNITTEST)
  enable_testing()
  add_subdirectory(unittest)
endif()

install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
