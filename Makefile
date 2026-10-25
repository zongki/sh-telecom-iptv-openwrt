#
# Copyright (C) 2026 zhangjunjie
#
# This is free software, licensed under the MIT License.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=sh-telecom-iptv
PKG_VERSION:=1.1.0
PKG_RELEASE:=12
PKG_LICENSE:=MIT
PKG_MAINTAINER:=zhangjunjie <zhangjunjie@local>

# Pure userspace daemon, no external source tarball: everything under src/.
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

# Compatibility policy: must build on OpenWrt 19.07 (gcc 7, musl 1.1,
# MHD 0.9.x) through 25.12 (gcc 14, musl 1.2, MHD 1.x).  Keep the code
# to C99 and the library API subset listed in DEPENDS; do not add
# version-guarded build flags here.
define Package/sh-telecom-iptv
  SECTION:=multimedia
  CATEGORY:=Multimedia
  TITLE:=Shanghai Telecom IPTV daemon (udpxy-compatible multicast relay)
  DEPENDS:=+libuci +libmicrohttpd
endef

define Package/sh-telecom-iptv/description
  IPTV soft-client daemon: udpxy-compatible UDP/RTP multicast-to-HTTP
  relay with UCI configuration and procd supervision.  The auth/EPG/
  timeshift/VOD modules are stubbed in this version and will be added
  incrementally.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) $(TARGET_LDFLAGS) \
		$(PKG_BUILD_DIR)/main.c $(PKG_BUILD_DIR)/config.c \
		$(PKG_BUILD_DIR)/log.c $(PKG_BUILD_DIR)/util.c \
		$(PKG_BUILD_DIR)/mcast.c $(PKG_BUILD_DIR)/httpd.c \
		$(PKG_BUILD_DIR)/httpc.c $(PKG_BUILD_DIR)/auth.c \
		$(PKG_BUILD_DIR)/json.c $(PKG_BUILD_DIR)/epg.c \
		$(PKG_BUILD_DIR)/ts.c $(PKG_BUILD_DIR)/vod.c \
		$(PKG_BUILD_DIR)/fcc.c $(PKG_BUILD_DIR)/xcode.c \
		$(PKG_BUILD_DIR)/crypto/md5.c $(PKG_BUILD_DIR)/crypto/aes.c \
		-o $(PKG_BUILD_DIR)/iptvd \
		-lmicrohttpd -luci -lpthread
endef

define Package/sh-telecom-iptv/conffiles
/etc/config/sh-telecom-iptv
endef

define Package/sh-telecom-iptv/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/etc/config $(1)/etc/init.d $(1)/etc/iptv
	$(INSTALL_DIR) $(1)/usr/share/sh-telecom-iptv/web $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/iptvd $(1)/usr/bin/
	$(INSTALL_CONF) ./files/sh-telecom-iptv.conf $(1)/etc/config/sh-telecom-iptv
	$(INSTALL_BIN) ./files/sh-telecom-iptv.init $(1)/etc/init.d/sh-telecom-iptv
	$(INSTALL_BIN) ./files/uci-defaults/99-sh-telecom-iptv-firewall $(1)/etc/uci-defaults/
	$(INSTALL_DATA) ./files/web/* $(1)/usr/share/sh-telecom-iptv/web/
endef

$(eval $(call BuildPackage,sh-telecom-iptv))
