#!/usr/bin/make -f
# Plugin DPF. Normalement appelé depuis le Makefile de la racine (DPF = sous-module ../dpf).
# Seul : make DPF_PATH=/chemin/vers/DPF clap   (ou vst3, lv2_sep, vst2...)
NAME = LowLatConv

DPF_PATH ?= ../dpf
ROOT = ..
DPF_BUILD_DIR = ../build
DPF_TARGET_DIR = ../bin

FILES_DSP = \
	LowLatConvPlugin.cpp \
	dr_wav_impl.cpp \
	pffft.c \
	pffft_common.c \
	fftpack.c

# sources de third_party/ trouvées par vpath : les objets restent à plat dans build/
vpath %.cpp $(ROOT)/third_party/dr_wav
vpath %.c   $(ROOT)/third_party/pffft/src

include $(DPF_PATH)/Makefile.plugins.mk

BUILD_C_FLAGS   += -I$(ROOT)/third_party/pffft/include -I$(ROOT)/third_party/pffft/src -w
BUILD_CXX_FLAGS += -std=c++17 -I$(ROOT) -I$(ROOT)/third_party/dr_wav -I$(ROOT)/third_party/pffft/include -I$(ROOT)/third_party/pffft/src -DLLC_USE_PFFFT
LINK_FLAGS      += -pthread

TARGETS += clap
all: $(TARGETS)
