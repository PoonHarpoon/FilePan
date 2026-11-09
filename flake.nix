# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

{
  description = "FilePan: development environment, and `nix run` to build the APK";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # Tracks Google's SDK repository daily, unlike nixpkgs' androidenv which lags behind.
    android-nixpkgs = {
      url = "github:tadfisher/android-nixpkgs/stable";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { nixpkgs, android-nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-darwin"
        "x86_64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
      perSystem =
        system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config.allowUnfree = true;
          };
          # Keep these in sync with app/build.gradle.
          buildToolsVersion = "37.0.0";
          androidSdk = android-nixpkgs.sdk.${system} (
            sdkPkgs: with sdkPkgs; [
              cmdline-tools-latest
              platform-tools
              build-tools-37-0-0
              platforms-android-37-2
              ndk-30-0-16248370
            ]
          );
          sdkRoot = "${androidSdk}/share/android-sdk";
          jdk = pkgs.jdk21;
          isLinux = pkgs.stdenv.hostPlatform.isLinux;
          # Mesa's lavapipe, a CPU Vulkan driver, so the renderer's host tests need no GPU.
          lavapipeIcd = "${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.${pkgs.stdenv.hostPlatform.uname.processor}.json";
          # What the Gradle build needs, shared by the dev shell and `nix run`.
          gradleEnv = {
            ANDROID_HOME = sdkRoot;
            ANDROID_SDK_ROOT = sdkRoot;
            JAVA_HOME = jdk.home;
            # AGP uses this CMake and Ninja from PATH: the SDK's CMake package ships a Ninja that
            # crashes on Nix, and AGP can't install its default CMake into the read-only store.
            FILEPAN_CMAKE_VERSION = pkgs.cmake.version;
            # The aapt2 AGP downloads from Maven is dynamically linked and won't run on NixOS.
            GRADLE_OPTS = "-Dorg.gradle.project.android.aapt2FromMavenOverride=${sdkRoot}/build-tools/${buildToolsVersion}/aapt2";
          };
          gradleTools = [
            androidSdk
            jdk
            pkgs.cmake
            pkgs.ninja
            pkgs.git
            # Generates the MIME type table from shared-mime-info at build time
            # (app/src/main/cpp/core/filepan/vfs/generate_mime_table.py, standard library only).
            # jinja2 and jsonschema are for Mbed TLS's source generators, which don't run by default
            # (GEN_FILES is off: the pinned release ships its generated files).
            (pkgs.python3.withPackages (ps: [
              ps.jinja2
              ps.jsonschema
              # utils/fonts/fetch-fonts.py instances the bundled variable fonts.
              ps.fonttools
            ]))
          ];
          # `nix run` builds the APK from the checkout it's run in (the build needs the git
          # submodules and writes to app/build, so it can't run from the store). Arguments replace
          # the default Gradle task: `nix run . -- assembleRelease`.
          buildApk = pkgs.writeShellApplication {
            name = "filepan-build-apk";
            runtimeInputs = gradleTools ++ [
              pkgs.coreutils
              pkgs.findutils
            ];
            text = ''
              ${pkgs.lib.concatStringsSep "\n" (
                pkgs.lib.mapAttrsToList (name: value: "export ${name}=${pkgs.lib.escapeShellArg value}") gradleEnv
              )}

              root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
              if [[ -z "$root" || ! -x "$root/gradlew" || ! -d "$root/app/src/main/cpp/core/filepan" ]]; then
                echo "filepan-build-apk: run this from inside a FilePan checkout" >&2
                exit 1
              fi
              cd "$root"
              if [[ ! -e third_party/libarchive/CMakeLists.txt ]]; then
                echo "Fetching the native dependencies (git submodules)..." >&2
                git submodule update --init --recursive
              fi

              tasks=("$@")
              if [[ ''${#tasks[@]} -eq 0 ]]; then
                tasks=(assembleDebug)
              fi
              stamp="$(mktemp)"
              trap 'rm -f "$stamp"' EXIT
              ./gradlew "''${tasks[@]}"

              apks="$(find app/build/outputs -name '*.apk' -newer "$stamp" 2>/dev/null || true)"
              if [[ -n "$apks" ]]; then
                echo
                echo "Built:"
                while IFS= read -r apk; do
                  echo "  $root/$apk"
                done <<< "$apks"
              fi
            '';
          };
        in
        {
          inherit buildApk;
          # clang and libc++ for host builds, so host tests accept exactly what the NDK's clang does.
          default = (pkgs.mkShell.override { stdenv = pkgs.llvmPackages_21.libcxxStdenv; }) ({
            packages = gradleTools ++ [
              # Host toolchain for building and benchmarking the native core off-device.
              pkgs.clang-tools
              # The golden images and README screenshots are LFS objects.
              pkgs.git-lfs
              # sshd for the SFTP client's host integration tests.
              pkgs.openssh
              # glslc for the renderer's shaders in host builds; Android builds use the NDK's.
              pkgs.shaderc
            ];
            # Libraries for host builds of the native core; the NDK sysroot provides them on Android.
            # ICU is the text system's bidi, line and grapheme breaking (the NDK has its own).
            buildInputs = [
              pkgs.zlib
              pkgs.icu
            ]
            ++ pkgs.lib.optionals isLinux [
              pkgs.vulkan-headers
              pkgs.vulkan-loader
            ];
          }
          // gradleEnv
          // pkgs.lib.optionalAttrs isLinux {
            # Points the Vulkan loader at lavapipe (newer loaders read the first name, older ones
            # the second), and makes the validation layers available to FILEPAN_VK_VALIDATION=1.
            VK_DRIVER_FILES = lavapipeIcd;
            VK_ICD_FILENAMES = lavapipeIcd;
            VK_ADD_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";
          });
        };
    in
    {
      devShells = forAllSystems (system: {
        inherit ((perSystem system)) default;
      });
      packages = forAllSystems (system: {
        default = (perSystem system).buildApk;
      });
      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${(perSystem system).buildApk}/bin/filepan-build-apk";
          meta.description = "Build FilePan's APK (assembleDebug by default) from this checkout";
        };
      });
    };
}
