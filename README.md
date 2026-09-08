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
| Front logo LED (PCA9634 @ i2c 0x54) | working: green once the boot script has run |
| Fan (EMC2104 @ i2c 0x2f) | working: vendor lookup table programmed at boot + FORCE_PWM/FORCE_12V pins on PCA9557@0x1c; off below 50 C. Stock kernel leaves it at 100% |
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

The image also carries ModemManager, the QMI/MBIM/serial-option USB drivers
and mwan3 for a Quectel mini-PCIe LTE module with WAN failover (uqmi is no
longer in the 25.12 repositories).

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

## Open items

1. **USB 3 ports (TI TUSB7340 xHCI).** SeaBIOS leaves the controller running
   and it ignores the halt request, so 6.12 fails `xhci_gen_setup` with -110.
   `patches/220-xhci-ti-tusb73x0-force-hcrst.patch` ports the vendor fix
   (force HCRST when the halt times out) but xhci is built into OpenWrt's x86
   kernel: applying it needs a full OpenWrt kernel build with the patch in
   `target/linux/x86/patches-6.12/`, not the SDK.  Low value: USB 2 works,
   the internal disk is USB 2, only the two blue sockets are affected.
2. **DSA for the two 88E6176.** Today they are unmanaged 4-port switches
   behind eth0/eth1.  mainline `mv88e6xxx` can drive them via platform data
   on the `igb-vc-*` buses (both `dsa_core.ko` and `mv88e6xxx.ko` are modules
   on x86, so this is SDK-buildable).  Needed:
   - a glue module creating an `mdio_device` at addr 0 on each bus with
     `struct dsa_mv88e6xxx_pdata` (compatible "marvell,mv88e6085", ports
     lan1-4 + cpu on port 4, ports 5/6 unused);
   - `net/dsa/dsa.c`: platform data hardcodes tree 0 / index 0, so the
     second switch fails with "tree 0 already setup" (~10 lines);
   - CPU port link: without a DT node DSA skips phylink for the CPU port and
     mv88e6xxx leaves port 4 unforced, so the SerDes link never comes up
     (~30-50 lines in DSA/mv88e6xxx to accept a fixed-link from platform
     data, or keep forcing port 4 from the init script after DSA setup);
   - drop the port-state part of `velo540-switch` (DSA owns it).
   Gains: lan1..lan8 as real interfaces (per-port link, VLANs, stats,
   hardware bridging inside each switch).  Traffic between the two switches
   still crosses the CPU unless ports 5/6 are described as DSA links.
3. **PoE (LTC4266 quad PSE).** Held in shutdown/reset by PCA9557@0x1c pins
   0/1 and not on the bus scan.  Enabling it is a few register writes (auto
   mode + detection enable per port) but it puts 48 V on four of the LAN
   ports and the vendor driver (`vendor-patches-3.14/996-*`, 2500 lines)
   manages a total current budget.  Not needed here; left alone on purpose.

Current port map: LAN1-4/LAN5-8 = two dumb 4-port switches on eth0/eth1
(LAN1 = switch B port 1); GE1/GE2 = eth4/eth5 (real NICs, 88E1514 PHY);
SFP1/SFP2 = eth2/eth3 (I350).  Both switch uplinks are in br-lan, so all
eight LAN ports are one L2 domain; traffic between the two groups crosses
the CPU.
