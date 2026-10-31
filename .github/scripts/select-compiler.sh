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
    echo "BASALT_CMAKE_ARGS=-DCMAKE_C_COMPILER=$llvm/bin/clang -DCMAKE_CXX_COMPILER=$llvm/bin/clang++ \"-DCMAKE_EXE_LINKER_FLAGS=$libs\" -DCMAKE_CXX_FLAGS=-D_LIBCPP_DISABLE_AVAILABILITY" >>"$GITHUB_ENV"
    ;;
*)
    echo "BASALT_CMAKE_ARGS=" >>"$GITHUB_ENV"
    ;;
esac
