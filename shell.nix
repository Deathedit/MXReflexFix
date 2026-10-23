# Cross-compile MXReflexFix.asi (32-bit Windows DLL) from NixOS.
#
#   nix-shell
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Output: build/MXReflexFix.asi
{ pkgs ? import <nixpkgs> { } }:

let
  cross = pkgs.pkgsCross.mingw32;
in
cross.mkShell {
  # Tools that run on the build machine.
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.git
  ];

  # Target-side libraries. The nixpkgs MinGW GCC uses the mcf thread model, so
  # its static archive has to be available for `-static` to fold it into the DLL.
  buildInputs = [
    cross.windows.mcfgthreads
    cross.windows.pthreads
  ];

  shellHook = ''
    echo "MXReflexFix cross shell: $(i686-w64-mingw32-g++ --version | head -n1)"
  '';
}
