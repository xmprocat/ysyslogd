#!/bin/bash
set -e

# Toolchains: gcc (native), or porting/<name>/porting.cmake for cross
toolchains=("${@:-gcc}")
src_dir="$(cd "$(dirname "$0")" && pwd)"
porting_dir="$src_dir/porting"

for toolchain in "${toolchains[@]}"; do
    build_dir="$src_dir/cmake_build_$toolchain"
    out_dir="$src_dir/out/$toolchain"

    rm -rf "$out_dir"
    mkdir -p "$out_dir"
    rm -rf "$build_dir"

    cmake_args=(
        -DCMAKE_BUILD_TYPE=Release
    )

    if [ "$toolchain" != "gcc" ]; then
        cmake_file="$porting_dir/$toolchain/porting.cmake"
        if [ -f "$cmake_file" ]; then
            cmake_args+=(-DCMAKE_TOOLCHAIN_FILE="$cmake_file")
        else
            echo "ERROR: toolchain cmake not found: $cmake_file"
            exit 1
        fi
    fi

    echo "=== Building for $toolchain ==="
    cmake "$src_dir" -B"$build_dir" "${cmake_args[@]}"
    cmake --build "$build_dir" --parallel "$(nproc)"

    cp "$build_dir/syslogd" "$out_dir/syslogd"

    echo ""
    echo "Build complete: $out_dir/syslogd"
    file "$out_dir/syslogd"
    echo ""
done
