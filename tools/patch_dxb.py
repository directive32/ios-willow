#!/usr/bin/env python3
"""
patch_dxb.py - Configure dxb/build.sh for Willow (ARMv8.0 Cortex-A73)
Willow Kryo 260 has NO ARMv8.1 LSE atomics (ldadd, cas, etc).
libfirehose_kernel must be built with -mcpu=cortex-a73+crypto -mno-outline-atomics.
"""
import os
import sys

def main():
    path = os.path.join("dxb", "build.sh")
    if not os.path.exists(path):
        print(f"Error: {path} not found!", file=sys.stderr)
        sys.exit(1)

    with open(path, "r", encoding="utf-8") as f:
        content = f.read()

    # 1. Update ARCHS and pass OTHER_CFLAGS to xcodebuild
    old_arch = 'ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"'
    new_arch = 'ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS="-mkernel -nostdinc -Wno-packed -mcpu=cortex-a73+crypto -mno-outline-atomics"'

    if old_arch not in content:
        print(f"Error: Could not find '{old_arch}' in {path}", file=sys.stderr)
        sys.exit(1)
    content = content.replace(old_arch, new_arch, 1)

    # 2. Patch libfirehose_kernel.xcconfig inside build_libdispatch()
    target_sed = 'sed -i \'\' \'s|$(SDKROOT)/usr/local/include|$(FAKEROOT_DIR)/usr/local/include|g\' "${SRCROOT}/xcodeconfig/libfirehose_kernel.xcconfig"'
    xcconfig_patch = target_sed + '\n        sed -i \'\' \'s|OTHER_CFLAGS = -mkernel -nostdinc -Wno-packed|OTHER_CFLAGS = -mkernel -nostdinc -Wno-packed -mcpu=cortex-a73+crypto -mno-outline-atomics|g\' "${SRCROOT}/xcodeconfig/libfirehose_kernel.xcconfig"'

    if target_sed not in content:
        print(f"Error: Could not find target_sed in {path}", file=sys.stderr)
        sys.exit(1)
    content = content.replace(target_sed, xcconfig_patch, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(content)

    print("Successfully patched dxb/build.sh for Cortex-A73 ARMv8.0-A!")

if __name__ == "__main__":
    main()
