#!/usr/bin/env bash
set -euo pipefail

case "$(uname -s)" in
Linux)
    wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
    sudo bash /tmp/llvm.sh 19
    sudo apt-get install -y clang-format-19
    echo "BASALT_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19" >>"$GITHUB_ENV"
    ;;
Darwin)
    brew install llvm
    llvm="$(brew --prefix llvm)"
    libs="-L$llvm/lib/c++ -Wl,-rpath,$llvm/lib/c++"
    cross=""
    if [ "${BASALT_TARGET_ARCH:-}" = x86_64 ] && [ "$(uname -m)" = arm64 ]; then
        sudo softwareupdate --install-rosetta --agree-to-license || true
        version="$(brew list --versions llvm | awk '{ print $2 }')"
        x64="$HOME/llvm-x64"
        if [ ! -f "$x64/built-$version" ]; then
            rm -rf "$x64" "$HOME/llvm-src"
            mkdir -p "$x64"
            git clone -q --depth 1 --filter=blob:none --sparse --branch "llvmorg-$version" https://github.com/llvm/llvm-project "$HOME/llvm-src"
            git -C "$HOME/llvm-src" sparse-checkout set runtimes libcxx libcxxabi libunwind cmake llvm/cmake llvm/utils third-party libc
            cmake -S "$HOME/llvm-src/runtimes" -B "$HOME/llvm-src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_C_COMPILER="$llvm/bin/clang" -DCMAKE_CXX_COMPILER="$llvm/bin/clang++" \
                -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_INSTALL_PREFIX="$x64" -DCMAKE_INSTALL_NAME_DIR=@rpath \
                -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF \
                -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=OFF
            cmake --build "$HOME/llvm-src/build" --target install-cxx install-cxxabi
            touch "$x64/built-$version"
        fi
        x64libs="$(dirname "$(find "$x64" -name libc++.1.dylib | head -1)")"
        file "$x64libs/libc++.1.dylib" | grep -q x86_64
        otool -L "$x64libs/libc++.1.dylib" "$x64libs/libc++abi.1.dylib"
        libs="-L$x64libs -Wl,-rpath,$x64libs"
        cross="-DCMAKE_OSX_ARCHITECTURES=x86_64"
    fi
    echo "BASALT_CMAKE_ARGS=-DCMAKE_C_COMPILER=$llvm/bin/clang -DCMAKE_CXX_COMPILER=$llvm/bin/clang++ \"-DCMAKE_EXE_LINKER_FLAGS=$libs\" -DCMAKE_CXX_FLAGS=-D_LIBCPP_DISABLE_AVAILABILITY $cross" >>"$GITHUB_ENV"
    ;;
*)
    echo "BASALT_CMAKE_ARGS=" >>"$GITHUB_ENV"
    ;;
esac
