# VeloCloud Edge 5X0 on OpenWrt 25.12 (kernel 6.12)

Stock OpenWrt 25.12.5 on the VeloCloud Edge 520 and Edge 540 (Intel Atom
C2000 "Rangeley" boards with two Marvell 88E6176 switches, two 88E1514 WAN
PHYs and two SFP cages), with a small, rebasable patch set instead of the
vendor's kernel fork.  Everything below is tested on an EDGE540 (C2558,
8 GB, fan) and an EDGE520 (C2358, 4 GB, fanless), both board rev 2.8.

Latest image: the newest `kernel-N` release on the
[releases page](https://github.com/gallops-gannets/velo540openwrt/releases).
Each carries a ready-to-write USB stick image (`openwrt-velo5x0-stick.img.gz`)
and the plain image for `sysupgrade`.

## Status

| | |
|---|---|
| LAN1-8 | **working**, per jack, as `lan1`..`lan8` under mainline DSA (mv88e6xxx); a plain two-dumb-switches mode is one command away |
| GE1 / GE2 (88E1514 behind I354) | **working** (`eth4` = WAN by default, `eth5`) |
| SFP1 / SFP2 (I354 SerDes, 1 Gbps only) | working (`eth2`, `eth3`) |
| USB 3 (TI TUSB7340) | working (`patches/220-*`, the controller needs a forced reset on 6.12) |
| WiFi (Atheros QCA988x mini-PCIe) | working (ath10k-ct) |
| Front logo RGB LED (PCA9634) | **working** as three OpenWrt LEDs `pca963x:{red,green,blue}:logo`, configurable in LuCI |
| Fan (EMC2104, Edge 540 only) | **working**: closed loop on the tach, off-at-idle band, tunable in `/etc/config/velo540` |
| TCO watchdog | working |
| Serial console | ttyS1, 115200 (the RJ45 console jack) |
| PoE | not populated on the tested units (no LTC4266 answers on any bus with its reset released); the control lines exist on PCA9557@0x1c pins 0/1 |
| Mini-PCIe slots | no USB on any slot: cellular modems must be USB devices |

## Installing on a stock Velo

1. Write the stick image to a USB stick (macOS shown; on Linux use `dd`):
   ```sh
   gunzip -c openwrt-velo5x0-stick.img.gz | sudo dd of=/dev/rdiskN bs=4m
   ```
2. Plug the stick into a **USB 2** socket (the black one; the blue USB 3
   sockets are dead until the OS is up), connect the console (115200) or a
   laptop on any LAN jack (the box's own network is 10.11.0.0/24), power on and pick the stick in the SeaBIOS boot
   menu (it sometimes needs a second Ctrl-Alt-Del before the stick is listed).
3. Log in (`root`, no password) at 10.11.0.1 or on the console and write
   the internal disk (the 7.6 GB USB disk, `sdb` when booted from the stick):
   ```sh
   wget -O /tmp/img.gz https://github.com/gallops-gannets/velo540openwrt/releases/download/kernel-N/openwrt-velo540-kernelbuild-ext4-combined.img.gz
   gunzip -c /tmp/img.gz | dd of=/dev/sdb bs=1M conv=fsync
   ```
   Then power off, remove the stick, power on.  Don't pull the stick while
   the box runs from it.
4. First boot: `velo540-dsa enable && reboot` for per-jack ports,
   `passwd`, and whatever else you need (Tailscale, WiFi, witness).

Later updates: `sysupgrade -v <image>.img.gz` from the running box keeps the
configuration.  Run it on the console or a foreground SSH session, not
backgrounded.

## Configuration knobs (all in `/etc/config/velo540`, kept across sysupgrade)

* **Switch mode**: `velo540-dsa enable|disable|status`.  DSA mode names the
  jacks `lan1`..`lan8` (measured map: LAN1=B1 LAN2=B0 LAN3=B3 LAN4=B2
  LAN5=A2 LAN6=A0 LAN7=A1 LAN8=A3; A = 00:14.0/eth0, B = 00:14.1/eth1).
  Unmanaged mode gives two 4-port dumb switches on `eth0` (LAN5-8) and `eth1`
  (LAN1-4).  Applied at boot by `velo540-switch` writing the glue module's
  `dsa_mask` parameter.
* **Fan** (`config fan`): `min_rpm`/`max_rpm` targets between `t_min` and
  `t_max`, `off_below`/`on_above` for an off-at-idle band, `tach auto|0`,
  `min_duty` for 2-wire fans.  The stock Sunon MF50101V1 (3-wire, voltage
  controlled by the board) cannot run below ~2700 RPM; a quiet replacement is
  the only way to a silent 540.  Fanless 520s skip the service.
* **Bad RAM pages** (`config memmap`, `list reserve '4K$0x...'`): re-applied
  to grub.cfg every boot by `velo540-memmap` (one tested 540 has a single
  weak cell that logs corrected ECC machine-checks).
* **Witness** (`config witness`): `list target 'name=ip'`, `period`, `fails`,
  `ntfy_url`, `pushover_token`/`pushover_user`.  Logs state changes, pages on
  down/recovered, lights the red logo LED while anything is down.
* **WiFi**: off by default; `velo540-wifi on|off|status`, and the failover hook
  brings it up while running on cellular.
* **Status JSON** for dashboards: `http://<box>/cgi-bin/status.json` (temps, fan,
  WAN/cellular, witness states, WiFi, LEDs); served on the LAN and Tailscale, and
  to the home LAN through firewall rule `velo_status_lan` if you add one.
* **LEDs**: standard OpenWrt LED config (`/etc/config/system`); defaults are
  green steady = all good, red = the witness sees something down, blue = running on cellular (hotplug hook, WAN down and `wwan`/`cellular` up).

Notes: `tailscale0` belongs in the `lan` firewall zone (fw4 rejects tunnel
traffic otherwise); mwan3 must stay disabled until a second WAN exists (its
policy rules preempt Tailscale's table 52, and stopping it empties that
table: restart tailscaled).  A `tailscale` mwan3 rule is pre-seeded.

## What the patches and glue do

* `patches/200-igb-velocloud-edge5x0.patch`: igb detects the board (NVM
  words 6/7 = "Vc"/"5X"), forces the 1000/full SGMII link with no PHY on
  functions 0/1 and exports their MDIO masters as `igb-vc-0000:00:14.{0,1}`,
  and routes PHY access on functions 2/3 to the external bit-bang bus.
* `patches/210-mdio-gpio-clear-level-before-input.patch`: gpio-ich on Avoton
  ORs cached output levels into reads; drive the line low before turnaround.
* `patches/220-xhci-ti-tusb73x0-force-hcrst.patch`: force HCRST when the
  TUSB7340 refuses to halt.
* `patches/230-dsa-pdata-own-tree.patch`: let each platform-data DSA switch
  have its own tree, so both 88E6176 register.
* `package/velo540-glue`: two out-of-tree modules.  `vc-edge5x0-mdio`
  sets the GPIO mux bits, registers the bit-bang MDIO bus (SoC GPIO 13/14 rev
  B, 11/12 rev A) and the board I2C bus (GPIO 11/12, adapter `i2c-9`) that
  carries the PCA9557 expanders, EMC2104, PCA9634 and EEPROM, and hands the
  PCA9634 to `leds-pca963x` through software nodes.  `vc-edge5x0-dsa` creates
  the mv88e6xxx platform-data devices on the igb-vc buses (`dsa_mask`
  writable at runtime).
* `files/etc/init.d/velo540-switch`: at boot, brings the switch CPU SerDes
  links up (and, in unmanaged mode, PHYs and forwarding), resets the 88E1514s
  through PCA9557@0x18 and re-probes their igb functions, sets the WAN PHY
  LEDs, puts the fan controller in PWM mode and enables the logo LED outputs.
* `files/usr/sbin/velo540-fand`, `velo540-witness`, `velo540-dsa`,
  `files/etc/init.d/velo540-{fan,memmap,witness}`: see above.

Hardware facts come from the vendor's GPL tree (`vendor-patches-3.14/`,
mirror of the dead `bitbucket.org/velocloud/openwrt`): I354 func 0/1 →
switch A/B port 4 SerDes, SMI on the function's own MDIO pins, single-chip
addressing; func 2/3 → 88E1514 at addr 0/1 on the GPIO bus; switch ports
0-3 = jacks, 4 = CPU, 5/6 = unused RGMII cross-links.

## Building

`.github/workflows/kernel.yml` (push to the `kernel` branch, or dispatch)
runs `scripts/kernel-build.sh`: a full OpenWrt v25.12.5 build with the
patches in `target/linux/x86/patches-6.12`, the glue as a kmod package, the
`files/` overlay, a 6 GB rootfs, console on ttyS1, `acpi_enforce_resources=lax`,
and the package set (LuCI, ModemManager, mwan3, Tailscale, python3, ser2net,
collectd, ath10k, USB serial drivers, i2c/mdio tools).  Results go to a
`kernel-<n>` release, including the uniquely signed stick image.

`scripts/build.sh` (`build-<n>` releases) is the older SDK + ImageBuilder
path; its modules do not load on `kernel-N` images (different kernel
config), so it is only useful against stock release kernels.

All `kernel-N` images share one MBR disk signature; that is why the stick
image is re-signed, and why a stick and the internal disk must never carry
the same one (the kernel would mount the wrong rootfs by PARTUUID).

## Not done / ideas

* PoE (no PSE found on these units).
* PXE/TFTP server for recovering other machines from the closet.
* ser2net consoles for other devices (packages are in the image, nothing attached yet).
* Cellular failover: ModemManager, mwan3 and the QMI/MBIM drivers are in the
  image; a USB modem (e.g. RM520N-GL in a USB carrier) is the plan, since no
  mini-PCIe slot has USB.
* Replacement fan for the 540: Sunon MF50101V3-1000U-G99 or any quiet 50x10 mm
  12 V 2- or 3-wire fan, spliced onto the existing connector.
