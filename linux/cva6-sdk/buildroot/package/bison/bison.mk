################################################################################
#
# bison
#
################################################################################

BISON_VERSION = 3.7.6
BISON_SOURCE = bison-$(BISON_VERSION).tar.xz
BISON_SITE = $(BR2_GNU_MIRROR)/bison
BISON_LICENSE = GPL-3.0+
BISON_LICENSE_FILES = COPYING
BISON_CPE_ID_VENDOR = gnu
# parallel build issue in examples/c/reccalc/
BISON_MAKE = $(MAKE1)
HOST_BISON_DEPENDENCIES = host-m4
HOST_BISON_CONF_OPTS = --enable-relocatable
# GCC 15 defaults to C23 + promotes these to errors; old gnulib in bison 3.7.6 trips both.
HOST_BISON_CONF_ENV = ac_cv_libtextstyle=no \
	CFLAGS="-O2 -std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"

$(eval $(host-autotools-package))
