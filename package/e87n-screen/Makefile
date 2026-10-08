#
# E87N 屏幕背光控制（纯脚本包，无编译产物）
#
# 来源：istoreos-for-edgepi-e87n 的 package/e87n-tools，只取屏幕那三件
# （screen-ctl / init.d/screen / config/screen）。风扇走内核 thermal 曲线，
# 没有用户态脚本包，见仓库根 README。
#
# 注意它走的是 GPIO524 直控背光，不是 pwm-backlight：
# istoreos 实测 pwm-backlight 驱动会抢占 GPIO524(PCM_MCK_I2S_MCLK/pwm2_0)，
# 既点不亮屏又让该脚无法 export。所以 DTS 里 pwm-backlight 节点保持 disabled，
# PWM2 不 mux，背光交给本包。
include $(TOPDIR)/rules.mk

PKG_NAME:=e87n-screen
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define Package/e87n-screen
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=EdgePi E87N screen backlight control (script only)
  DEPENDS:=+kmod-fb-tft-nv3007
  PKGARCH:=all
endef

define Package/e87n-screen/description
  E87N 屏幕背光控制。screen-ctl 直控 GPIO524 开关背光、整屏填充、
  导出当前帧；init.d/screen 开机点亮。
  依赖 kmod-fb-tft-nv3007 提供 /dev/fb0。
endef

define Build/Compile
endef

define Package/e87n-screen/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/usr/sbin/screen-ctl $(1)/usr/sbin/screen-ctl
	$(INSTALL_BIN) ./files/usr/sbin/screen-test $(1)/usr/sbin/screen-test
	$(INSTALL_BIN) ./files/usr/sbin/screen-status $(1)/usr/sbin/screen-status
	$(INSTALL_BIN) ./files/etc/init.d/screen $(1)/etc/init.d/screen
	$(INSTALL_DATA) ./files/etc/config/screen $(1)/etc/config/screen
endef

$(eval $(call BuildPackage,e87n-screen))
