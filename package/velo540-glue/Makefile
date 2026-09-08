include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=velo540-glue
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/velo540-glue
  SUBMENU:=Other modules
  TITLE:=VeloCloud Edge 5x0 board glue (GPIO MDIO/I2C buses)
  DEPENDS:=@TARGET_x86 +kmod-mdio-gpio +kmod-i2c-gpio +kmod-gpio-pca953x
  FILES:=$(PKG_BUILD_DIR)/vc-edge5x0-mdio.ko
  AUTOLOAD:=$(call AutoLoad,30,vc-edge5x0-mdio,1)
endef

define KernelPackage/velo540-glue/description
  Registers the bit-bang MDIO bus for the 88E1514 WAN PHYs and the I2C bus
  for the board expanders/fan/LED on a VeloCloud Edge 520/540.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,velo540-glue))
