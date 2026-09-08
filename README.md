# VeloCloud Edge 540 on OpenWrt 25.12 (kernel 6.12)

Get the 10 RJ45 ports of a VeloCloud Edge 520/540 working on stock OpenWrt
25.12.5 with a small, rebasable patch set instead of the vendor's kernel fork.

Status (tested on an EDGE540 board rev 2.8):

| | |
|---|---|
| LAN1-8 (2x 88E6176 behind I354 func 0/1) | **working**, as two unmanaged 4-port switches on eth0/eth1, both in br-lan (LAN1 = switch B port 1) |
| SFP1/SFP2 (I350) | working (eth2, eth3) |
| GE1/GE2 (88E1514 behind I354 func 2/3) | **working** (eth4 = wan, eth5): PCA9557 reset pulse + patched mdio-gpio |
| TCO watchdog | working (kmod-itco-wdt) |
| USB 3 ports (TI TUSB7340) | broken on 6.12: controller does not halt; `patches/220-*` is a port of the vendor fix but xhci is built into the OpenWrt x86 kernel, so it needs a full kernel build, not the SDK |
| DSA / per-port control | not yet (mv88e6xxx via platform data, needs a small DSA-core patch for two trees) |

Hardware facts come from the vendor's GPL tree (`vendor-patches-3.14/`, mirror
of the dead `bitbucket.org/velocloud/openwrt`):

| I354 function | wired to |
|---|---|
| 0000:00:14.0 | 88E6176 switch A, port 4 SerDes; switch SMI on the function's own MDIO pins |
| 0000:00:14.1 | 88E6176 switch B, same |
| 0000:00:14.2 | 88E1514 PHY (WAN0 RJ45) on a GPIO bit-bang MDIO bus, PHY addr 0 |
| 0000:00:14.3 | 88E1514 PHY (WAN1 RJ45), same bus, PHY addr 1 |

Each switch: ports 0-3 = LAN RJ45, 4 = CPU link, 5/6 = RGMII cross-links.

## What is built

* `patches/200-igb-velocloud-edge5x0.patch` - igb: detect the board (NVM words
  6/7 = "Vc"/"5X"), forced 1000/full SGMII link with no PHY on functions 0/1
  and export their MDIO master as `mii_bus` `igb-vc-0000:00:14.{0,1}`,
  route PHY access on functions 2/3 to the external bus named by module
  parameter `vc_ext_mdio` (default `gpio-0`).
* `glue/vc-edge5x0-mdio.c` - registers `mdio-gpio` on SoC GPIO 13/14 (rev B)
  or 11/12 (rev A) via `gpio-ich`, giving the bus the 88E1514s live on.
* `patches/210-mdio-gpio-clear-level-before-input.patch` - gpio-ich on Avoton
  caches output levels and ORs them into reads; without this every odd PHY
  register on the bit-bang bus reads 0xffff.
* `files/etc/init.d/velo540-switch` - runs at boot: SerDes/PHY/port-state
  setup on both switches, PCA9557 reset pulse for the 88E1514s, re-probe of
  the WAN NICs.  Also the place to look for the register sequence.
* `files/etc/inittab`, `files/etc/uci-defaults/50-velo540-network` - shell on
  ttyS1; eth0+eth1 in the LAN bridge, SFP1 as WAN.
* `scripts/build.sh` - SDK build of the modules + ImageBuilder image with the
  modules overlaid and extra packages, grub console on ttyS1 with
  `acpi_enforce_resources=lax`, unique MBR disk signature (the internal disk
  usually carries the stock one and the kernel would mount the wrong rootfs).

GitHub Actions runs `scripts/build.sh` on every push and attaches the results
to a release named `build-<n>`: the image, the two `.ko` files, and `build.log`.

## Using an image

```
gunzip openwrt-25.12.5-x86-64-velo540-ext4-combined.img.gz
sudo dd if=openwrt-25.12.5-x86-64-velo540-ext4-combined.img of=/dev/rdiskN bs=4m
```

Legacy BIOS boot, console ttyS1 115200 (already set in grub.cfg).  Put the
stick in a USB **2** port: the USB 3 ports hang off the broken xHCI and Linux
will not see the stick there (SeaBIOS will).  SeaBIOS often lists the stick
only on the second boot attempt.  First things to check on the box:

```
dmesg | grep -i "custom link\|igb-vc\|vc-edge5x0"
mdio igb-vc-0000:00:14.0 raw 0x13 3      # 88E6176 product id, expect 0x176x
mdio igb-vc-0000:00:14.0 mvls
i2cdetect -y 0                           # PCA9557 reset expanders at 0x18/0x1c
```
