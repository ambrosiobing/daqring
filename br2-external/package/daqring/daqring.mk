################################################################################
#
# daqring - the repo itself is the source tree (site method "local")
#
################################################################################

DAQRING_VERSION = local
DAQRING_SITE = $(BR2_EXTERNAL_DAQRING_PATH)/..
DAQRING_SITE_METHOD = local
DAQRING_LICENSE = GPL-2.0
DAQRING_LICENSE_FILES = README.md

# The kernel module is built by the kernel-module infrastructure (the
# top-level Makefile's KERNELRELEASE branch provides obj-m). The test
# client is cross-compiled here; for the 64-bit acquire load
# (see load_head in the test client), so no libatomic is needed.
define DAQRING_BUILD_CMDS
	$(TARGET_CC) -O2 -Wall -Wextra -I$(@D)/include \
		-o $(@D)/daqring_test $(@D)/test/daqring_test.c
endef

define DAQRING_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/daqring_test \
		$(TARGET_DIR)/usr/bin/daqring_test
endef

$(eval $(kernel-module))
$(eval $(generic-package))
