YSYSLOGD_SITE = $(TOPDIR)/package/ysyslogd
YSYSLOGD_SITE_METHOD = local
YSYSLOGD_VERSION = 1.0

define YSYSLOGD_CONFIGURE_CMDS
	mkdir -p $(@D)/build
	cd $(@D)/build && cmake $(@D)/syslogd \
		-DCMAKE_TOOLCHAIN_FILE=$(HOST_DIR)/share/buildroot/toolchainfile.cmake \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DCMAKE_BUILD_TYPE=Release
endef

define YSYSLOGD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/build
endef

define YSYSLOGD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/syslogd $(TARGET_DIR)/usr/sbin/ysyslogd
	$(INSTALL) -D -m 0644 $(@D)/syslogd.conf $(TARGET_DIR)/etc/syslog.conf
	$(INSTALL) -D -m 0644 $(@D)/ysyslogd.service \
		$(TARGET_DIR)/usr/lib/systemd/system/ysyslogd.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/sysinit.target.wants
	ln -sf /usr/lib/systemd/system/ysyslogd.service \
		$(TARGET_DIR)/etc/systemd/system/sysinit.target.wants/ysyslogd.service
	# Run depmod so modprobe works for all kernel modules
	$(HOST_DIR)/sbin/depmod -a -b $(TARGET_DIR) 6.1.118+
endef

$(eval $(generic-package))
