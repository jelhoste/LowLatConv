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

# Garde-fou : message clair si ce fichier n'est pas dans plugin/ ou si DPF est absent de ../dpf
ifeq ($(wildcard $(DPF_PATH)/Makefile.plugins.mk),)
$(error DPF introuvable dans "$(DPF_PATH)". Ce Makefile doit rester dans le dossier plugin/ et DPF doit etre dans ../dpf)
endif

include $(DPF_PATH)/Makefile.plugins.mk

BUILD_C_FLAGS   += -I$(ROOT)/third_party/pffft/include -I$(ROOT)/third_party/pffft/src -DPFFFT_STATIC_DEFINE -w
BUILD_CXX_FLAGS += -std=c++17 -I$(ROOT) -I$(ROOT)/third_party/dr_wav -I$(ROOT)/third_party/pffft/include -I$(ROOT)/third_party/pffft/src -DPFFFT_STATIC_DEFINE -DLLC_USE_PFFFT

# DPF's Mutex / Thread use pthread (winpthreads) on Windows, but DPF only adds -lpthread to the CLAP link when a UI is
# built (CLAP_LIBS). Without this line a DSP-only Windows build fails with 'undefined reference to pthread_mutex_lock'.
ifeq ($(WINDOWS),true)
EXTRA_DSP_LIBS += -lpthread
endif

TARGETS += clap
all: $(TARGETS)
