# tuxpad - Makefile
#
# Build:            make
# Run:              make run
# Install (system): sudo make install     (to /usr/local, with desktop entry)
# Install (user):   make install-user     (to ~/.local, no root needed)
# Uninstall:        sudo make uninstall  /  make uninstall-user
# Clean:            make clean

CC      ?= gcc
PKGS     = gtk+-3.0 gtksourceview-3.0 gmodule-2.0
CFLAGS  += -O2 -Wall -Wextra -std=c11 -Iinclude $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) -lm -rdynamic

GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   = $(shell pkg-config --libs gtk+-3.0)

PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
DATADIR  = $(PREFIX)/share/tuxpad
LIBDIR   = $(PREFIX)/lib/tuxpad
APPSDIR  = $(PREFIX)/share/applications
ICONDIR  = $(PREFIX)/share/icons/hicolor/scalable/apps

# Desktop folder (honours localised names, e.g. ~/Schreibtisch)
DESKTOP_DIR ?= $(shell xdg-user-dir DESKTOP 2>/dev/null)
ifeq ($(strip $(DESKTOP_DIR)),)
DESKTOP_DIR := $(HOME)/Desktop
endif

TARGET   = tuxpad
SRC      = src/tuxpad.c
SCHEMA   = schemas/gschemas.compiled

PLUGIN_SRCS = $(wildcard plugins/*.c)
PLUGINS     = $(PLUGIN_SRCS:.c=.so)

all: $(TARGET) $(SCHEMA) $(PLUGINS)

$(TARGET): $(SRC) include/tuxpad-plugin.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Compiled compatibility schema (GTK3/GNOME-schema workaround)
$(SCHEMA): schemas/org.gnome.settings-daemon.plugins.xsettings.gschema.xml
	glib-compile-schemas schemas/

# Example plugins (shared objects)
plugins/%.so: plugins/%.c include/tuxpad-plugin.h
	$(CC) -shared -fPIC -Iinclude $(GTK_CFLAGS) -o $@ $< $(GTK_LIBS)

plugins: $(PLUGINS)

run: all
	./$(TARGET)

# ---- System-wide installation (needs root) --------------------------------
install: all
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 $(SCHEMA) $(DESTDIR)$(DATADIR)/schemas/gschemas.compiled
	install -d $(DESTDIR)$(LIBDIR)/plugins
	for so in $(PLUGINS); do install -Dm755 $$so $(DESTDIR)$(LIBDIR)/plugins/`basename $$so`; done
	install -d $(DESTDIR)$(APPSDIR)
	sed -e 's|^Exec=tuxpad|Exec=$(BINDIR)/tuxpad|' \
	    -e 's|^TryExec=tuxpad|TryExec=$(BINDIR)/tuxpad|' \
	    data/tuxpad.desktop > $(DESTDIR)$(APPSDIR)/tuxpad.desktop
	chmod 644 $(DESTDIR)$(APPSDIR)/tuxpad.desktop
	install -Dm644 data/tuxpad.svg $(DESTDIR)$(ICONDIR)/tuxpad.svg
	-update-desktop-database $(DESTDIR)$(APPSDIR) 2>/dev/null || true
	-gtk-update-icon-cache -f -t $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null || true
	@echo "Installed tuxpad to $(BINDIR). It should now appear in your app menu."

# Place a launcher icon on the desktop
desktop-icon:
	install -d "$(DESKTOP_DIR)"
	sed -e 's|^Exec=tuxpad|Exec=$(BINDIR)/tuxpad|' \
	    -e 's|^TryExec=tuxpad|TryExec=$(BINDIR)/tuxpad|' \
	    data/tuxpad.desktop > "$(DESKTOP_DIR)/tuxpad.desktop"
	chmod +x "$(DESKTOP_DIR)/tuxpad.desktop"
	-gio set "$(DESKTOP_DIR)/tuxpad.desktop" metadata::trusted true 2>/dev/null || true
	-gio set "$(DESKTOP_DIR)/tuxpad.desktop" metadata::xdg.origin.url "" 2>/dev/null || true
	@echo "Desktop icon placed in $(DESKTOP_DIR)."

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(APPSDIR)/tuxpad.desktop
	rm -f $(DESTDIR)$(ICONDIR)/tuxpad.svg
	rm -f "$(DESKTOP_DIR)/tuxpad.desktop"
	rm -rf $(DESTDIR)$(DATADIR) $(DESTDIR)$(LIBDIR)
	-update-desktop-database $(DESTDIR)$(APPSDIR) 2>/dev/null || true

# ---- Per-user installation (no root) --------------------------------------
install-user:
	$(MAKE) install PREFIX=$(HOME)/.local
	$(MAKE) desktop-icon PREFIX=$(HOME)/.local
	@echo "Make sure $(HOME)/.local/bin is on your PATH."

uninstall-user:
	$(MAKE) uninstall PREFIX=$(HOME)/.local

clean:
	rm -f $(TARGET) $(SCHEMA) $(PLUGINS)

.PHONY: all plugins run install uninstall install-user uninstall-user \
        desktop-icon clean
