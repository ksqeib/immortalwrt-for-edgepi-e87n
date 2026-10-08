#
# E87N 面板状态页渲染器（C，自绘像素，写 /dev/fb0）
#
# 为什么不用内核 fbcon：那是单色文本层，字体是内核编死的 8x16，改不了颜色、
# 字号、布局，做不出仪表盘。本包自带字模（src/e87n-font.h，由 tools/raster-font.js
# 从 Oswald.ttf 栅格化而来），直接 mmap /dev/fb0 画像素，可做彩色大字号。
#
# 为什么不抄 EN87 的 display-control：那包里带一个 2.1 MB 的专有 AArch64 二进制
# 和一个 libfreetype 依赖。本包不链 FreeType（字模已烘成 C 数组）、不带任何
# 第三方二进制，源码全部可审。
#
# 字体许可：Oswald.ttf 为 SIL Open Font License 1.1，见 files/usr/share/e87n-display/OFL.txt
include $(TOPDIR)/rules.mk

PKG_NAME:=e87n-display
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0
PKG_LICENSE_FILES:=files/usr/share/e87n-display/OFL.txt
PKG_MAINTAINER:=EdgePi E87N port

include $(INCLUDE_DIR)/package.mk

define Package/e87n-display
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=EdgePi E87N panel status renderer (color, direct fb)
  DEPENDS:=+kmod-fb-tft-nv3007
endef

define Package/e87n-display/description
  E87N 面板状态页。直接写 /dev/fb0 画彩色的大字号仪表盘：温度、CPU、内存、
  网口速率、客户端数。自带从 Oswald.ttf 栅格化的点阵字模，不依赖 FreeType，
  同时提供 text 模式（走内核 fbcon）作为回退。
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-Wall -Wextra -O2 \
		-o $(PKG_BUILD_DIR)/e87n-display ./src/e87n-display.c
endef

define Package/e87n-display/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/share/e87n-display
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/e87n-display $(1)/usr/sbin/e87n-display
	$(INSTALL_DATA) ./files/usr/share/e87n-display/Oswald.ttf $(1)/usr/share/e87n-display/Oswald.ttf
	$(INSTALL_DATA) ./files/usr/share/e87n-display/OFL.txt $(1)/usr/share/e87n-display/OFL.txt
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/etc/init.d/e87n-display $(1)/etc/init.d/e87n-display
	$(INSTALL_CONF) ./files/etc/config/e87n-display $(1)/etc/config/e87n-display
endef

define Package/e87n-display/conffiles
/etc/config/e87n-display
endef

$(eval $(call BuildPackage,e87n-display))
