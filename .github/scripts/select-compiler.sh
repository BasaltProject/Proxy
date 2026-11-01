#!/usr/bin/env bash
set -euo pipefail

case "$(uname -s)" in
Linux)
    echo 'Acquire::http::Timeout "30"; Acquire::Retries "3";' | sudo tee /etc/apt/apt.conf.d/99basalt >/dev/null
    wget -qO /tmp/llvm.sh --tries=3 --timeout=30 https://apt.llvm.org/llvm.sh
    sudo timeout 900 bash /tmp/llvm.sh 19
    sudo timeout 300 apt-get install -y clang-format-19
    echo "BASALT_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19" >>"$GITHUB_ENV"
    ;;
Darwin)
    brew install llvm
    llvm="$(brew --prefix llvm)"
    target=13.0
    echo "MACOSX_DEPLOYMENT_TARGET=$target" >>"$GITHUB_ENV"
    arch="${BASALT_TARGET_ARCH:-$(uname -m)}"
    cross=""
    if [ "$arch" = x86_64 ] && [ "$(uname -m)" = arm64 ]; then
        sudo softwareupdate --install-rosetta --agree-to-license || true
        cross="-DCMAKE_OSX_ARCHITECTURES=x86_64"
    fi
    version="$(brew list --versions llvm | awk '{ print $2 }')"
    prefix="$HOME/libcxx-$arch"
    if [ ! -f "$prefix/built-$version-$target" ]; then
        rm -rf "$prefix" "$HOME/src-llvm"
        mkdir -p "$prefix"
        git clone -q --depth 1 --filter=blob:none --sparse --branch "llvmorg-$version" https://github.com/llvm/llvm-project "$HOME/src-llvm"
        git -C "$HOME/src-llvm" sparse-checkout set runtimes libcxx libcxxabi libunwind cmake llvm/cmake llvm/utils third-party libc
        cmake -S "$HOME/src-llvm/runtimes" -B "$HOME/src-llvm/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER="$llvm/bin/clang" -DCMAKE_CXX_COMPILER="$llvm/bin/clang++" \
            -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$target" -DCMAKE_INSTALL_PREFIX="$prefix" \
            -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF \
            -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
            -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF
        cmake --build "$HOME/src-llvm/build" --target install-cxx install-cxxabi
        touch "$prefix/built-$version-$target"
    fi
    cxx="$(find "$prefix" -name libc++.a | head -1)"
    lipo -archs "$cxx" | grep -q "$arch"
    echo "BASALT_CMAKE_ARGS=-DCMAKE_C_COMPILER=$llvm/bin/clang -DCMAKE_CXX_COMPILER=$llvm/bin/clang++ -DCMAKE_OSX_DEPLOYMENT_TARGET=$target \"-DCMAKE_CXX_STANDARD_LIBRARIES=-lc++ $cxx\" -DCMAKE_CXX_FLAGS=-D_LIBCPP_DISABLE_AVAILABILITY $cross" >>"$GITHUB_ENV"
    ;;
*)
    echo "BASALT_CMAKE_ARGS=" >>"$GITHUB_ENV"
    ;;
esac
