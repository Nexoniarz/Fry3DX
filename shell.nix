# Dev shell for NixOS: `nix-shell`, then `cmake --preset debug && cmake --build build/debug`.
# Windows builds too (cmake --preset windows, see cmake/toolchains/windows-msvc.cmake);
# Android: android/shell.nix.
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    cmake ninja gcc pkg-config git
    mold ccache
    shaderc                       # glslc
    sdl3
    vulkan-loader vulkan-validation-layers vulkan-tools
    llvmPackages_19.clang-unwrapped llvmPackages_19.lld llvmPackages_19.llvm xwin
  ];
  LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (with pkgs; [ vulkan-loader sdl3 ]);
  VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";
  # clang's own headers for the Windows cross compiler (Nix keeps them outside its resource dir).
  KEEL_CLANG_INCLUDE = "${pkgs.llvmPackages_19.libclang.lib}/lib/clang/19/include";
}
