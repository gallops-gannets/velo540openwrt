#!/bin/bash
# Full OpenWrt build (kernel + packages + image) with the VeloCloud patches
# applied in-tree.  Runs on an x86_64 Linux host, ~2 h on 4 cores.
set -euo pipefail
REL=${REL:-v25.12.5}
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$PWD/out}; mkdir -p "$OUT"
[ -d openwrt ] || git clone --depth 1 --branch "$REL" https://github.com/openwrt/openwrt.git openwrt
cd openwrt
./scripts/feeds update -a >/dev/null && ./scripts/feeds install -a >/dev/null

# kernel patches: igb glue, mdio-gpio workaround, xhci quirk
cp "$REPO"/patches/200-igb-velocloud-edge5x0.patch \
   "$REPO"/patches/210-mdio-gpio-clear-level-before-input.patch \
   "$REPO"/patches/220-xhci-ti-tusb73x0-force-hcrst.patch \
   target/linux/x86/patches-6.12/
# glue as a kernel package, rootfs overlay
rm -rf package/velo540-glue && cp -r "$REPO"/package/velo540-glue package/
rm -rf files && cp -r "$REPO"/files files && rm -f files/etc/inittab   # TARGET_SERIAL handles the shell

cat > .config <<CFG
CONFIG_TARGET_x86=y
CONFIG_TARGET_x86_64=y
CONFIG_TARGET_x86_64_DEVICE_generic=y
CONFIG_TARGET_SERIAL="ttyS1"
CONFIG_GRUB_BOOTOPTS="acpi_enforce_resources=lax"
CONFIG_TARGET_ROOTFS_EXT4FS=y
# CONFIG_TARGET_ROOTFS_SQUASHFS is not set
# CONFIG_GRUB_EFI_IMAGES is not set
CONFIG_PACKAGE_kmod-velo540-glue=y
CFG
for p in kmod-igb kmod-itco-wdt kmod-i2c-i801 kmod-gpio-pca953x kmod-mdio-gpio kmod-i2c-gpio kmod-dsa-mv88e6xxx \
	 kmod-usb-storage-uas kmod-usb3 kmod-hwmon-coretemp i2c-tools mdio-tools kmod-mdio-netlink ethtool tcpdump-mini gpiod-tools \
	 kmod-usb-net-qmi-wwan kmod-usb-net-cdc-mbim kmod-usb-serial-option kmod-usb-acm modemmanager mwan3 \
	 luci luci-ssl luci-proto-modemmanager luci-app-mwan3 luci-app-attendedsysupgrade luci-app-package-manager \
	 kmod-ath10k-ct ath10k-firmware-qca988x-ct wpad-basic-mbedtls iw; do
	echo "CONFIG_PACKAGE_$p=y" >> .config
done
make defconfig >/dev/null
grep -E "^CONFIG_TARGET_SERIAL|^CONFIG_GRUB_BOOTOPTS|velo540|CONFIG_PACKAGE_kmod-usb3=" .config

make -j"$(nproc)" download >/dev/null
make -j"$(nproc)" || make -j1 V=s
ls -la bin/targets/x86/64/
cp bin/targets/x86/64/*ext4-combined.img.gz "$OUT/openwrt-velo540-kernelbuild-ext4-combined.img.gz"
cp bin/targets/x86/64/*.manifest "$OUT/" 2>/dev/null || true
cp bin/targets/x86/64/kernel-debug.tar.zst "$OUT/" 2>/dev/null || true
(cd "$OUT" && sha256sum * > sha256sums)
