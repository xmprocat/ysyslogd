set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_PREFIX aarch64-openwrt-linux-musl)
set(TOOLCHAIN_DIR /opt/immortalwrt-sdk-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64/staging_dir/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl)
set(TARGET_DIR /opt/immortalwrt-sdk-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64/staging_dir/target-aarch64_cortex-a53_musl)

set(CMAKE_C_COMPILER "${TOOLCHAIN_DIR}/bin/${TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_C_FLAGS "-Os -pipe -mcpu=cortex-a53 -fno-caller-saves -fno-plt -fhonour-copts -fmacro-prefix-map=${CMAKE_SOURCE_DIR}= -Wno-unused-result")

set(CMAKE_FIND_ROOT_PATH "${TOOLCHAIN_DIR}" "${TARGET_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
