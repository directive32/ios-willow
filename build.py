#!/usr/bin/env python3
"""Build script for Willow (Redmi Note 8T) boot shim with GZIP compression."""
import os
import subprocess
import gzip

CLANG = r"C:\Program Files\LLVM\bin\clang.exe"
LLD = r"C:\Program Files\LLVM\bin\ld.lld.exe"
OBJCOPY = r"C:\Program Files\LLVM\bin\llvm-objcopy.exe"
NM = r"C:\Program Files\LLVM\bin\llvm-nm.exe"
MKBOOTIMG = os.path.join("tools", "mkbootimg.py")

OUT_DIR = os.path.join("shim_willow", "out")
os.makedirs(OUT_DIR, exist_ok=True)

# 1. Compile assembly and C sources
cflags = [
    "--target=aarch64-none-elf", "-O2", "-ffreestanding", "-nostdlib",
    "-fno-pic", "-mgeneral-regs-only", "-mstrict-align", "-fno-stack-protector",
    "-Wall", "-Wextra", "-Ishim_willow"
]

sources = ["start.S", "console.c", "mmu.c", "fdt.c", "loader.c", "shim.c"]
objs = []
for src in sources:
    obj = os.path.join(OUT_DIR, src.rsplit(".", 1)[0] + ".o")
    objs.append(obj)
    src_path = os.path.join("shim_willow", src)
    subprocess.run([CLANG] + cflags + ["-c", src_path, "-o", obj], check=True)

# 2. Link with lld
elf_path = os.path.join(OUT_DIR, "shim.elf")
link_cmd = [
    LLD, "-T", os.path.join("shim_willow", "shim.ld"),
    "--build-id=none", "-nostdlib", "-static", "-o", elf_path
] + objs
subprocess.run(link_cmd, check=True)

# 3. Convert to raw Image
img_path = os.path.join(OUT_DIR, "Image")
subprocess.run([OBJCOPY, "-O", "binary", elf_path, img_path], check=True)

# 4. Pad Image to _end
res = subprocess.run([NM, elf_path], capture_output=True, text=True, check=True)
size = None
for line in res.stdout.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[2] == "_end":
        size = int(parts[0], 16)
        break
assert size is not None, "Failed to find _end symbol"
with open(img_path, "r+b") as f:
    f.truncate(size)
print(f"Padded Image to {size} bytes ({size // 1024} KB).")

# 5. Gzip Image and append DTB (standard Qualcomm Image.gz-dtb format)
gz_img_path = os.path.join(OUT_DIR, "Image.gz-dtb")
with open(img_path, "rb") as f:
    raw_img = f.read()
gz_data = gzip.compress(raw_img, 9)

with open(os.path.join("dtb", "live.dtb"), "rb") as f:
    dtb_data = f.read()

with open(gz_img_path, "wb") as f:
    f.write(gz_data + dtb_data)
print(f"Created Image.gz-dtb ({len(gz_data) + len(dtb_data)} bytes).")

# 6. Create empty ramdisk for diagnostic image
rd_empty_path = os.path.join(OUT_DIR, "ramdisk_empty.cpio.gz")
magic = b'070701'
fields = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0]
entry = magic + b''.join(f'{x:08x}'.encode() for x in fields) + b'TRAILER!!!\0'
entry += b'\0' * ((4 - (len(entry) % 4)) % 4)
entry += b'\0' * ((512 - (len(entry) % 512)) % 512)
with open(rd_empty_path, "wb") as f:
    f.write(gzip.compress(entry, 9))

# 7. Package diagnostic boot image (no payload)
clean_boot = os.path.join(OUT_DIR, "shim-willow-clean.img")
subprocess.run([
    "python", MKBOOTIMG,
    "--header_version", "1",
    "--kernel", gz_img_path,
    "--ramdisk", rd_empty_path,
    "--base", "0",
    "--kernel_offset", "0x8000",
    "--ramdisk_offset", "0x1000000",
    "--second_offset", "0xf00000",
    "--tags_offset", "0x100",
    "--pagesize", "4096",
    "--cmdline", "console=null",
    "-o", clean_boot
], check=True)
print(f">>> Generated diagnostic image: {clean_boot}")

# 8. Package XNU boot image (payload.bin as ramdisk)
payload_path = os.path.join(OUT_DIR, "payload.bin")
if os.path.exists(payload_path):
    xnu_boot = os.path.join(OUT_DIR, "shim-willow-xnu.img")
    subprocess.run([
        "python", MKBOOTIMG,
        "--header_version", "1",
        "--kernel", gz_img_path,
        "--ramdisk", payload_path,
        "--base", "0",
        "--kernel_offset", "0x8000",
        "--ramdisk_offset", "0x1000000",
        "--second_offset", "0xf00000",
        "--tags_offset", "0x100",
        "--pagesize", "4096",
        "--cmdline", "console=null",
        "-o", xnu_boot
    ], check=True)
    print(f">>> Generated XNU boot image: {xnu_boot} ({os.path.getsize(xnu_boot) // (1024*1024)} MB)")
