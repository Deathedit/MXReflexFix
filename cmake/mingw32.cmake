# Toolchain for i686-w64-mingw32 (provided by shell.nix, or Ubuntu's mingw-w64 packages in CI).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(TOOLCHAIN_PREFIX i686-w64-mingw32)
# Ubuntu ships the compilers as i686-w64-mingw32-g++-posix / -win32; pass -DMINGW_SUFFIX=-posix there.
set(MINGW_SUFFIX "" CACHE STRING "Suffix of the MinGW gcc/g++ binaries")

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc${MINGW_SUFFIX})
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++${MINGW_SUFFIX})
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}-objdump CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
