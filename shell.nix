# Dev shell for NixOS: `nix-shell`, then `cmake --preset debug && cmake --build build/debug`.
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    cmake ninja gcc pkg-config git
    mold ccache
    shaderc                       # glslc
    sdl3
    vulkan-loader vulkan-validation-layers vulkan-tools
  ];
  LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (with pkgs; [ vulkan-loader sdl3 ]);
  VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";
}
