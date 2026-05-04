include $(TOPDIR)/rules.mk

PKG_NAME:=ysyslogd
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_SOURCE_DIR = $(CURDIR)

define Package/ysyslogd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Standalone syslog daemon with ring buffer
  DEPENDS:=+librt
endef

define Package/ysyslogd/description
  Standalone syslog daemon extracted from busybox 1.36.1.
  Features: shared-memory ring buffer (crash-safe), TCP/UDP remote
  forwarding, TCP/UDP syslog server, log reader mode, runtime
  configuration via Unix socket control channel.
endef

define Package/ysyslogd/conffiles
/etc/syslog.conf
endef

define Package/ysyslogd/install
	$(INSTALL_DIR) $(1)/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/sbin/syslogd $(1)/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/syslogd.init $(1)/etc/init.d/ysyslogd
	$(INSTALL_DIR) $(1)/etc
	$(INSTALL_CONF) ./files/syslogd.conf $(1)/etc/syslog.conf
endef

$(eval $(call BuildPackage,ysyslogd))
