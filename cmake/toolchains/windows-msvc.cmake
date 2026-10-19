# Cross-compiling for Windows (x64) from Linux with clang-cl + lld-link against the real MSVC CRT
# and Windows SDK, laid out by xwin:
#   xwin --accept-license --arch x86_64 splat --output ~/.xwin
#   cmake --preset windows && cmake --build --preset windows
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER ${CMAKE_CURRENT_LIST_DIR}/clang-cl-xwin)
set(CMAKE_CXX_COMPILER ${CMAKE_CURRENT_LIST_DIR}/clang-cl-xwin)
set(CMAKE_LINKER ${CMAKE_CURRENT_LIST_DIR}/lld-link-xwin)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_CXX_FLAGS_INIT "/EHsc")
if(DEFINED ENV{XWIN_DIR})
  set(XWIN "$ENV{XWIN_DIR}")
else()
  set(XWIN "$ENV{HOME}/.xwin")
endif()
set(CMAKE_RC_FLAGS_INIT "-I${XWIN}/sdk/include/um -I${XWIN}/sdk/include/shared")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR wine)
# Never pick up the build machine's libraries (e.g. a Linux SDL3): fetch and build everything.
set(CMAKE_FIND_ROOT_PATH "${XWIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
