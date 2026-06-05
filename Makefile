PLUGIN_NAME = smart_url
SRC = smart_url.c
BUILD_DIR = build
INSTALL_DIR = $(HOME)/.local/share/profanity/plugins

CC ?= gcc
CFLAGS ?= -Wall -Wextra
PROFANITY_LIBS = "-lprofanity"
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0 2>/dev/null || echo "")
GLIB_LIBS = $(shell pkg-config --libs glib-2.0 2>/dev/null || echo "-lglib-2.0")
STROPHE_CFLAGS = $(shell pkg-config --cflags libstrophe 2>/dev/null || echo "")
STROPHE_LIBS = $(shell pkg-config --libs libstrophe 2>/dev/null || echo "-lstrophe")

# Clipboard support via GTK3 (default: enabled)
# Build without: make WITHOUT_CLIPBOARD=1
ifndef WITHOUT_CLIPBOARD
HAS_CLIPBOARD = 1
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null || echo "")
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0 2>/dev/null || echo "-lgtk-3")
CFLAGS += -DHAS_CLIPBOARD
endif

all: $(BUILD_DIR)/$(PLUGIN_NAME).so

$(BUILD_DIR)/$(PLUGIN_NAME).so: $(SRC)
	mkdir -p $(BUILD_DIR)
	$(CC) -shared -o $@ -fPIC $(CFLAGS) $(GLIB_CFLAGS) $(STROPHE_CFLAGS) $(GTK_CFLAGS) -Wl,-rpath=$(LIBRARY_PATH) $< $(PROFANITY_LIBS) $(GLIB_LIBS) $(STROPHE_LIBS) $(GTK_LIBS)

install: all
	mkdir -p $(INSTALL_DIR)
	cp $(BUILD_DIR)/$(PLUGIN_NAME).so $(INSTALL_DIR)/

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all install clean