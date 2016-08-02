#
# Makefile for Asterisk flite application
# Copyright (C) 2009 - 2016, Lefteris Zafiris
#
# This program is free software, distributed under the terms of
# the GNU General Public License Version 2. See the COPYING file
# at the top of the source tree.

INSTALL:=install
ASTLIBDIR:=$(shell awk '/moddir/{print $$3}' /etc/asterisk/asterisk.conf)
ifeq ($(strip $(ASTLIBDIR)),)
	MODULES_DIR:=$(INSTALL_PREFIX)/usr/lib/asterisk/modules
else
	MODULES_DIR:=$(INSTALL_PREFIX)$(ASTLIBDIR)
endif
ASTETCDIR:=$(INSTALL_PREFIX)/etc/asterisk
SAMPLENAME:=flite.conf.sample
CONFNAME:=$(basename $(SAMPLENAME))

CC=gcc
OPTIMIZE=-O2
DEBUG=-g

LIBS+=-lflite_cmu_us_kal -lflite_usenglish -lflite_cmulex -lflite
CFLAGS+=-pipe -fPIC -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -D_REENTRANT -D_GNU_SOURCE

all: app_flite.so
	@echo " +--------- app_flite Build Complete --------+"
	@echo " + app_flite has successfully been built,    +"
	@echo " + and can be installed by running:          +"
	@echo " +                                           +"
	@echo " +               make install                +"
	@echo " +-------------------------------------------+"

app_flite.o: app_flite.c
	$(CC) $(CFLAGS) $(DEBUG) $(OPTIMIZE) -c -o app_flite.o app_flite.c

app_flite.so: app_flite.o
	$(CC) -shared -Xlinker -x -o $@ $< $(LIBS)

clean:
	rm -f app_flite.o app_flite.so

install: all
	$(INSTALL) -m 755 -d $(DESTDIR)$(MODULES_DIR)
	$(INSTALL) -m 755 app_flite.so $(DESTDIR)$(MODULES_DIR)
	@echo " +---- app_flite Installation Complete ------+"
	@echo " +                                           +"
	@echo " + app_flite has successfully been installed.+"
	@echo " + If you would like to install the sample   +"
	@echo " + configuration file run:                   +"
	@echo " +                                           +"
	@echo " +              make samples                 +"
	@echo " +-------------------------------------------+"

samples:
	@mkdir -p $(DESTDIR)$(ASTETCDIR)
	@if [ -f $(DESTDIR)$(ASTETCDIR)/$(CONFNAME) ]; then \
		echo "Backing up previous config file as $(CONFNAME).old";\
		mv -f $(DESTDIR)$(ASTETCDIR)/$(CONFNAME) $(DESTDIR)$(ASTETCDIR)/$(CONFNAME).old ; \
	fi ;
	$(INSTALL) -m 644 $(SAMPLENAME) $(DESTDIR)$(ASTETCDIR)/$(CONFNAME)
	@echo " ------- app_flite confing Installed ---------"
