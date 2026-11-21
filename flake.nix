{
  description = "Quant compiler development environment";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        clang = pkgs.clangStdenv.cc;

        cmake = pkgs.cmake;
        ninja = pkgs.ninja;
        bear = pkgs.bear;
        ccache = pkgs.ccache;
        mold = pkgs.mold;
        lld = pkgs.lld;

        python3 = pkgs.python3;
        python3Packages = pkgs.python3Packages;

        devShell = pkgs.mkShell {
          name = "quant-dev";
          nativeBuildInputs = [
            cmake
            ninja
            bear
            ccache
            mold
            lld
            python3
            python3Packages.pyyaml
            python3Packages.tomli
          ];
          buildInputs = [
            clang
            pkgs.zlib
            pkgs.openssl
          ];
          CMAKE_CXX_COMPILER = clang;
          CMAKE_C_COMPILER = pkgs.clangStdenv.cc;
          CMAKE_LINKER = "mold";
          CMAKE_CXX_FLAGS = "-stdlib=libc++";
          CMAKE_EXE_LINKER_FLAGS = "-stdlib=libc++ -fuse-ld=mold";
          CMAKE_MODULE_LINKER_FLAGS = "-stdlib=libc++ -fuse-ld=mold";
          CMAKE_SHARED_LINKER_FLAGS = "-stdlib=libc++ -fuse-ld=mold";
        };
      in
      {
        devShells.default = devShell;

        packages.default = pkgs.stdenv.mkDerivation {
          name = "quant";
          src = self;
          nativeBuildInputs = [ cmake ninja ];
          buildInputs = [ clang pkgs.zlib pkgs.openssl ];
          dontUseCmakeConfigure = true;
          buildPhase = ''
            cmake -B build -G Ninja .
            cmake --build build
          '';
          installPhase = ''
            cmake --install build --prefix $out
          '';
        };
      }
    );
}