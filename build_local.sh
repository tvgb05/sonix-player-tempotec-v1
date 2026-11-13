#!/bin/bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

echo "=========================================="
echo " TempoTec V1 Local Firmware Build"
echo "=========================================="

# 1. Chuẩn bị firmware gốc nếu chưa có
echo "-> [1/4] Checking stock firmware base..."
chmod +x sonix-packer/*.sh 2>/dev/null || true
if [ ! -f "sonix-packer/v1_original.upt" ]; then
    echo "Extracting stock v1_original.upt from bundled archives..."
    sonix-packer/prepare_v1_stock.sh "sonix-packer/firmware-input/v1.zip.part1" "sonix-packer"
else
    echo "Stock v1_original.upt is present."
fi

# 2. Biên dịch sonix_player & sonix_launch cho chip MIPS
echo "-> [2/4] Compiling MIPS target binaries..."
cd "$REPO_DIR/sonix-player"
chmod +x rockboxdev/*.sh 2>/dev/null || true
make target -j"$(nproc)"

# 3. Copy binary sang thư mục packer
echo "-> [3/4] Installing target binaries..."
install -m 0755 sonix_player "$REPO_DIR/sonix-packer/sonix_player"
install -m 0755 sonix_launch "$REPO_DIR/sonix-packer/sonix_launch"

# 4. Đóng gói firmware v1.upt
echo "-> [4/4] Packing firmware image..."
cd "$REPO_DIR/sonix-packer"
./sonix_firmware_packer.sh

# 5. Copy ra thư mục MOD Firmware nếu tồn tại
OUT_DIR="/mnt/e/TempotecV1_MOD_Firmware"
if [ ! -d "$OUT_DIR" ] && [ -d "E:/TempotecV1_MOD_Firmware" ]; then
    OUT_DIR="E:/TempotecV1_MOD_Firmware"
fi

if [ -d "$OUT_DIR" ]; then
    echo "-> Deploying to $OUT_DIR..."
    cp -f v1.upt v1_md5.txt "$OUT_DIR/"
fi

echo "=========================================="
echo " BUILD SUCCESSFUL!"
echo " Firmware: $REPO_DIR/sonix-packer/v1.upt"
if [ -f "$REPO_DIR/sonix-packer/v1_md5.txt" ]; then
    cat "$REPO_DIR/sonix-packer/v1_md5.txt"
fi
echo "=========================================="
