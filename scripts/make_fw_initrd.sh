#!/usr/bin/env bash
# scripts/make_fw_initrd.sh
# Empacota os firmwares da AMD Polaris10 (compativeis com Polaris20/RX 590 GME)
# em um arquivo CPIO para ser carregado pelo GRUB como modulo Multiboot2.
set -e

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$ROOT_DIR/build/fw_staging"
OUTPUT="$ROOT_DIR/build/fw.cpio"
FW_SRC="/lib/firmware/amdgpu"

FW_LIST=(
    polaris10_pfp.bin
    polaris10_me.bin
    polaris10_ce.bin
    polaris10_mec.bin
    polaris10_rlc.bin
    polaris10_sdma.bin
    polaris10_sdma1.bin
    polaris10_mc.bin
)

echo ">> Preparando staging em $OUTDIR..."
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/lib/firmware/amdgpu"
mkdir -p "$(dirname "$OUTPUT")"

echo ">> Descomprimindo firmware blobs..."
for fw in "${FW_LIST[@]}"; do
    src="$FW_SRC/${fw}.zst"
    dst="$OUTDIR/lib/firmware/amdgpu/$fw"
    if [ -f "$src" ]; then
        zstd -d "$src" -o "$dst" -f -q
        size=$(stat -c%s "$dst")
        echo "   [OK] $fw ($size bytes)"
    else
        echo "   [SKIP] $src nao encontrado"
    fi
done

echo ">> Gerando CPIO em $OUTPUT..."
cd "$OUTDIR"
find . | cpio --create --format=newc --quiet > "$OUTPUT"
cd - > /dev/null

size=$(stat -c%s "$OUTPUT")
echo ">> fw.cpio gerado: $size bytes"
