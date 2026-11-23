#!/usr/bin/env bash
set -euo pipefail

case "$(uname -s)" in
Linux)
    echo 'Acquire::http::Timeout "30"; Acquire::Retries "3";' | sudo tee /etc/apt/apt.conf.d/99basalt >/dev/null
    installed=0
    for attempt in 1 2 3 4; do
        if sudo timeout 300 apt-get update &&
            sudo timeout 600 apt-get install -y clang-19 clang-tools-19 lld-19 libclang-rt-19-dev; then
            installed=1
            break
        fi
        # Downloaded packages are kept, so a retry resumes on the main archive instead of the slow runner mirror.
        if [ -f /etc/apt/apt-mirrors.txt ]; then
            echo https://archive.ubuntu.com/ubuntu/ | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
        fi
    done
    [ "$installed" = 1 ]
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
    version="${version%%_*}"
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
MINGW* | MSYS* | CYGWIN*)
    args=""
    llvm_major() { clang --version 2>/dev/null | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1; }
    arm64=0
    if [ "${PROCESSOR_ARCHITECTURE:-}" = ARM64 ] || [ "${PROCESSOR_ARCHITEW6432:-}" = ARM64 ]; then
        arm64=1
    fi
    # The MSVC STL on the runner image rejects a Clang older than 22.
    major="$(llvm_major)"
    if [ "${major:-0}" -lt 22 ]; then
        choco upgrade llvm --no-progress -y || true
        export PATH="/c/Program Files/LLVM/bin:$PATH"
        echo "C:\\Program Files\\LLVM\\bin" >>"$GITHUB_PATH"
        major="$(llvm_major)"
    fi
    if [ "${major:-0}" -lt 22 ]; then
        # Chocolatey answers 503 now and then and still exits 0, so take the installer from the LLVM release.
        version=22.1.8
        installer="LLVM-$version-win64.exe"
        if [ "$arm64" = 1 ]; then
            installer="LLVM-$version-woa64.exe"
        fi
        curl -fsSL --proto "=https" --tlsv1.2 --retry 5 --retry-all-errors --retry-delay 10 -o "$RUNNER_TEMP/llvm.exe" \
            "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$installer"
        powershell -NoProfile -Command "Start-Process -FilePath '$(cygpath -w "$RUNNER_TEMP/llvm.exe")' -ArgumentList '/S','/D=C:\\LLVM' -Wait"
        export PATH="/c/LLVM/bin:$PATH"
        echo "C:\\LLVM\\bin" >>"$GITHUB_PATH"
        major="$(llvm_major)"
    fi
    if [ "${major:-0}" -lt 22 ]; then
        echo "Clang ${major:-none} is older than 22 and could not be upgraded" >&2
        exit 1
    fi
    if ! command -v ninja >/dev/null; then
        choco install ninja --no-progress -y
    fi
    if [ "$arm64" = 1 ]; then
        # An emulated x64 Clang targets x64 unless told otherwise.
        args="-DCMAKE_C_COMPILER_TARGET=aarch64-pc-windows-msvc -DCMAKE_CXX_COMPILER_TARGET=aarch64-pc-windows-msvc"
    fi
    echo "BASALT_CMAKE_ARGS=$args" >>"$GITHUB_ENV"
    ;;
*)
    echo "BASALT_CMAKE_ARGS=" >>"$GITHUB_ENV"
    ;;
esac
