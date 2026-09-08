# VeloCloud Edge 540 on OpenWrt 25.12 (kernel 6.12)

Work in progress: get the 10 RJ45 ports of a VeloCloud Edge 520/540 working on
stock OpenWrt 25.12.5 with a small, rebasable patch set instead of the vendor's
kernel fork.  **Nothing here has been run on hardware yet.**

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
* `scripts/build.sh` - SDK build of the two modules + ImageBuilder image with
  the modules overlaid and extra packages, grub console switched to ttyS1.

GitHub Actions runs `scripts/build.sh` on every push and attaches the results
to a release named `build-<n>`: the image, the two `.ko` files, and `build.log`.

## Using an image

```
gunzip openwrt-25.12.5-x86-64-velo540-ext4-combined.img.gz
sudo dd if=openwrt-25.12.5-x86-64-velo540-ext4-combined.img of=/dev/rdiskN bs=4m
```

Legacy BIOS boot, console ttyS1 115200 (already set in grub.cfg).  First things
to check on the box:

```
dmesg | grep -i "custom link\|igb-vc\|vc-edge5x0"
mdio igb-vc-0000:00:14.0 raw 0x13 3      # 88E6176 product id, expect 0x176x
mdio igb-vc-0000:00:14.0 mvls
i2cdetect -y 0                           # PCA9557 reset expanders at 0x18/0x1c
```
