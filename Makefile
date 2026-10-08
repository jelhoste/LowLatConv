#!/usr/bin/make -f
# Root Makefile used by the DPF GitHub action (and for local builds).
#   git submodule add https://github.com/DISTRHO/DPF dpf       (once)
#   make                 -> builds the plugin into bin/
#   make -f Makefile.tests check   -> DSP test suite (no DPF needed)

include dpf/Makefile.base.mk

all: dgl plugins

# ---------------------------------------------------------------------------------------------------------------------

# DGL (the UI toolkit) is only needed once the plugin has a UI: set LLC_WITH_UI=true then (and DISTRHO_PLUGIN_HAS_UI 1).
LLC_WITH_UI ?= false

dgl:
ifeq ($(LLC_WITH_UI),true)
ifeq ($(HAVE_DGL),true)
	$(MAKE) -C dpf/dgl
endif
endif

plugins: dgl
	$(MAKE) all -C plugin

# ---------------------------------------------------------------------------------------------------------------------

clean:
	$(MAKE) clean -C plugin
	rm -rf bin build

.PHONY: all dgl plugins clean
