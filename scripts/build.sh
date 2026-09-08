#!/bin/bash
# Build a VeloCloud Edge 540 OpenWrt image on an x86_64 Linux host (GitHub
# Actions runner or any box with sudo, zstd, build-essential, libelf-dev).
#
#  1. OpenWrt SDK      -> patched in-tree igb.ko + out-of-tree vc-edge5x0-mdio.ko
#  2. OpenWrt ImageBuilder -> ext4-combined (legacy BIOS) image with the extra
#     packages and the two modules overlaid into /lib/modules/<kver>/
#  3. grub.cfg fixed for this board's console (ttyS1 / serial unit 1)
#
# Output: out/
set -euo pipefail
REL=${REL:-25.12.5}
BASE=https://downloads.openwrt.org/releases/${REL}/targets/x86/64
SDK=openwrt-sdk-${REL}-x86-64_gcc-14.3.0_musl.Linux-x86_64
IB=openwrt-imagebuilder-${REL}-x86-64.Linux-x86_64
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$PWD/work}
OUT=${OUT:-$PWD/out}
mkdir -p "$WORK" "$OUT"
cd "$WORK"

fetch() { [ -d "$1" ] || { echo "== fetching $1"; curl -sSL -o "$1.tar.zst" "$BASE/$1.tar.zst"; tar --zstd -xf "$1.tar.zst"; }; }

############ 1. kernel modules via SDK ############
fetch "$SDK"
LINUX=$(find "$WORK/$SDK/build_dir" -maxdepth 3 -type d -name 'linux-6.*' | head -1)
[ -n "$LINUX" ] || { echo "no kernel tree in SDK"; find "$WORK/$SDK/build_dir" -maxdepth 3 | head; exit 1; }
KVER=$(basename "$LINUX" | sed 's/^linux-//')
TOOL=$(find "$WORK/$SDK/staging_dir" -maxdepth 1 -type d -name 'toolchain-x86_64_*' | head -1)
export STAGING_DIR="$WORK/$SDK/staging_dir"
export PATH="$TOOL/bin:$STAGING_DIR/host/bin:$PATH"
CROSS=x86_64-openwrt-linux-musl-
echo "== kernel $KVER in $LINUX, toolchain $(basename "$TOOL")"
grep -E '^CONFIG_(IGB|MODVERSIONS|OBJTOOL|MODULE_SIG)\b' "$LINUX/.config" || true

IGB="$LINUX/drivers/net/ethernet/intel/igb"
if [ ! -f "$IGB/igb_main.c" ]; then
	echo "== SDK kernel tree has no igb sources; fetching v$KVER from kernel.org"
	mkdir -p "$IGB"
	for f in Makefile e1000_82575.c e1000_82575.h e1000_defines.h e1000_hw.h e1000_i210.c \
		 e1000_i210.h e1000_mac.c e1000_mac.h e1000_mbx.c e1000_mbx.h e1000_nvm.c e1000_nvm.h \
		 e1000_phy.c e1000_phy.h e1000_regs.h igb.h igb_ethtool.c igb_hwmon.c igb_main.c igb_ptp.c; do
		curl -sSL -o "$IGB/$f" "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/net/ethernet/intel/igb/$f?h=v$KVER"
	done
fi
if ! grep -q igb_vc "$IGB/Makefile"; then
	echo "== applying igb patch"
	(cd "$LINUX" && patch -p1 --forward < "$REPO"/patches/200-igb-velocloud-edge5x0.patch)
fi
############ 2a. ImageBuilder pass 1: unpack the release kmods ############
# The SDK's Module.symvers does not attribute module-exported symbols
# (libphy, i2c-core, ptp, hwmon...) to their modules, so a plain M= build
# ends up with an empty "depends=" and kmodloader would insert igb before
# its dependencies.  Build the image once, harvest the real .ko files, and
# hand modpost a symvers that names them.
fetch "$IB"
PACKAGES="kmod-igb kmod-libphy kmod-itco-wdt kmod-i2c-i801 kmod-gpio-pca953x kmod-mdio-gpio kmod-dsa-mv88e6xxx
          kmod-usb-storage-uas kmod-usb3 kmod-hwmon-coretemp i2c-tools mdio-tools kmod-mdio-netlink ethtool tcpdump-mini"
echo "== ImageBuilder pass 1: $PACKAGES"
make -C "$WORK/$IB" image PROFILE=generic PACKAGES="$(echo $PACKAGES)" 2>&1 | tail -15
MODDIR=$(dirname "$(find "$WORK/$IB/build_dir" -name libphy.ko | head -1)")
echo "== release modules in $MODDIR: $(ls "$MODDIR" | wc -l) files"
EXTRA="$WORK/extra.symvers"; : > "$EXTRA"
for ko in "$MODDIR"/*.ko; do
	m=$(basename "$ko" .ko)
	"${CROSS}nm" "$ko" 2>/dev/null | awk -v m="$m" '$3 ~ /^__ksymtab_/ { s=$3; sub(/^__ksymtab_/, "", s); printf "0x00000000\t%s\t%s\tEXPORT_SYMBOL\t\n", s, m }'
done >> "$EXTRA"
echo "== extra.symvers: $(wc -l < "$EXTRA") symbols; sample:"; grep -wE 'mdiobus_alloc|i2c_bit_add_bus|ptp_clock_register' "$EXTRA"
echo "== SDK Module.symvers says:"; grep -wE 'mdiobus_alloc|i2c_bit_add_bus|ptp_clock_register' "$LINUX/Module.symvers" || echo "(not present)"
# drop the SDK's own (module-less) entries for symbols we now attribute to modules
cp "$LINUX/Module.symvers" "$LINUX/Module.symvers.orig"
awk 'NR==FNR { have[$2]=1; next } !($2 in have)' "$EXTRA" "$LINUX/Module.symvers.orig" > "$LINUX/Module.symvers"

############ 1b. kernel modules ############
echo "== building igb.ko"
make -C "$LINUX" ARCH=x86 CROSS_COMPILE="$CROSS" CONFIG_IGB=m M=drivers/net/ethernet/intel/igb KBUILD_EXTRA_SYMBOLS="$EXTRA" modules
cp "$IGB/igb.ko" "$OUT/"

echo "== building vc-edge5x0-mdio.ko"
rm -rf "$WORK/glue" && cp -r "$REPO/glue" "$WORK/glue"
make -C "$LINUX" ARCH=x86 CROSS_COMPILE="$CROSS" M="$WORK/glue" KBUILD_EXTRA_SYMBOLS="$EXTRA" modules
cp "$WORK/glue/vc-edge5x0-mdio.ko" "$OUT/"
for k in "$OUT"/*.ko; do echo "-- $(basename "$k")"; "${CROSS}strip" --strip-debug "$k"; modinfo "$k" | grep -E '^(vermagic|depends|parm)'; done
echo "-- stock igb.ko for comparison:"; modinfo "$MODDIR/igb.ko" | grep -E '^(vermagic|depends)'

############ 2b. ImageBuilder pass 2: with the modules overlaid ############
FILES="$WORK/files"; rm -rf "$FILES"; cp -r "$REPO/files" "$FILES"
mkdir -p "$FILES/lib/modules/$KVER"
cp "$OUT"/igb.ko "$OUT"/vc-edge5x0-mdio.ko "$FILES/lib/modules/$KVER/"
echo "== ImageBuilder pass 2"
make -C "$WORK/$IB" image PROFILE=generic PACKAGES="$(echo $PACKAGES)" FILES="$FILES" 2>&1 | tail -15
IMG=$(ls "$WORK/$IB"/bin/targets/x86/64/*-generic-ext4-combined.img.gz | head -1)
[ -n "$IMG" ] || { echo "no ext4-combined image produced"; ls -la "$WORK/$IB"/bin/targets/x86/64/; exit 1; }

############ 3. grub console -> ttyS1 ############
RAW="$WORK/velo540.img"; gunzip -c "$IMG" > "$RAW"
MNT="$WORK/mnt"; mkdir -p "$MNT"
LOOP=$(sudo losetup -Pf --show "$RAW")
sudo mount "${LOOP}p1" "$MNT"
sudo sed -i 's/--unit=0/--unit=1/; s/console=ttyS0,/console=ttyS1,/g' "$MNT/boot/grub/grub.cfg"
echo "== grub.cfg:"; sudo cat "$MNT/boot/grub/grub.cfg"
sudo umount "$MNT"; sudo losetup -d "$LOOP"
gzip -9 -c "$RAW" > "$OUT/openwrt-${REL}-x86-64-velo540-ext4-combined.img.gz"
cp "$WORK/$IB"/bin/targets/x86/64/*.manifest "$OUT/" 2>/dev/null || true
sha256sum "$OUT"/* | tee "$OUT/sha256sums"
ls -la "$OUT"
