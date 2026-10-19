# Android build environment for Fry3DX: SDK (platform 35, build-tools), NDK, adb, a JDK.
#   nix-shell android/shell.nix --run "python3 android/build_apk.py"
{ pkgs ? import <nixpkgs> { config = { android_sdk.accept_license = true; allowUnfree = true; }; } }:
let
  android = pkgs.androidenv.composeAndroidPackages {
    platformVersions = [ "35" ];
    buildToolsVersions = [ "35.0.0" ];
    includeNDK = true;
    ndkVersions = [ "27.2.12479018" ];
    includeEmulator = false;
    includeSystemImages = false;
  };
  sdk = android.androidsdk;
in pkgs.mkShell {
  packages = [ sdk pkgs.jdk17 pkgs.cmake pkgs.ninja pkgs.python3 pkgs.glslang pkgs.shaderc pkgs.zip pkgs.unzip ];
  ANDROID_HOME = "${sdk}/libexec/android-sdk";
  ANDROID_SDK_ROOT = "${sdk}/libexec/android-sdk";
  ANDROID_NDK_ROOT = "${sdk}/libexec/android-sdk/ndk/27.2.12479018";
  JAVA_HOME = "${pkgs.jdk17.home}";
}
