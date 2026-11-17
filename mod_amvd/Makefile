# mod_amvd - build out of tree against an installed FreeSWITCH
#
#   make                       # uses pkg-config freeswitch (set PKG_CONFIG_PATH if needed)
#   make FS_INCLUDES=/usr/local/freeswitch/include/freeswitch FS_MODDIR=/usr/local/freeswitch/mod
#   sudo make install
#
# Debian/Ubuntu SignalWire packages: apt install libfreeswitch-dev  (pkg-config name: freeswitch)

PKG_CONFIG    ?= pkg-config
FS_INCLUDES   ?= $(shell $(PKG_CONFIG) --variable=includedir freeswitch 2>/dev/null)
FS_MODDIR     ?= $(shell $(PKG_CONFIG) --variable=modulesdir freeswitch 2>/dev/null)
FS_CONFDIR    ?= $(shell $(PKG_CONFIG) --variable=confdir freeswitch 2>/dev/null)
FS_CFLAGS     ?= $(shell $(PKG_CONFIG) --cflags freeswitch 2>/dev/null)
FS_MODDIR     := $(if $(FS_MODDIR),$(FS_MODDIR),/usr/lib/freeswitch/mod)
FS_CONFDIR    := $(if $(FS_CONFDIR),$(FS_CONFDIR),/etc/freeswitch)

ENGINE_DIR    := ../engine
CC            ?= gcc
CFLAGS        ?= -O2 -g
CFLAGS        += -std=gnu99 -fPIC -Wall -Wextra -Wno-unused-parameter \
                 $(FS_CFLAGS) $(if $(FS_INCLUDES),-I$(FS_INCLUDES)) -I$(ENGINE_DIR)
LDFLAGS       += -shared -Wl,--version-script=mod_amvd.map -Wl,-z,defs -lm
# -z,defs needs libfreeswitch at link time; drop it if you build without the .so present
FS_LIBS       ?= $(shell $(PKG_CONFIG) --libs freeswitch 2>/dev/null)

all: mod_amvd.so

mod_amvd.so: mod_amvd.o amvd_engine.o mod_amvd.map
	$(CC) -o $@ mod_amvd.o amvd_engine.o $(LDFLAGS) $(FS_LIBS)

mod_amvd.o: mod_amvd.c $(ENGINE_DIR)/amvd_engine.h
	$(CC) $(CFLAGS) -c -o $@ $<

amvd_engine.o: $(ENGINE_DIR)/amvd_engine.c $(ENGINE_DIR)/amvd_engine.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: mod_amvd.so
	install -D -m 0755 mod_amvd.so $(DESTDIR)$(FS_MODDIR)/mod_amvd.so
	@if [ ! -f $(DESTDIR)$(FS_CONFDIR)/autoload_configs/amvd.conf.xml ]; then \
		install -D -m 0644 conf/amvd.conf.xml $(DESTDIR)$(FS_CONFDIR)/autoload_configs/amvd.conf.xml; \
		echo "installed default amvd.conf.xml"; else echo "kept existing amvd.conf.xml"; fi

clean:
	rm -f *.o *.so

.PHONY: all install clean
