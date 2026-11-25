APP_NAME   := aap_ctl
# The AirPods pictures are Apple's artwork (see ../../NOTICE.md). APPLE_ART=0 builds without them: the page then
# draws plain stand-ins (./install.sh --no-apple-art also leaves out the icon art).
APPLE_ART  ?= 1
ART_SRC    := $(if $(filter 1,$(APPLE_ART)),aap_art.c,aap_art_none.c)
SRCS       := aap_ctl.c aap_ui.c aap_models.c $(ART_SRC) aap_icons.c aap_font_14.c aap_font_16.c aap_font_20.c aap_snap.c
LV_SURFACE := 1
# Render into a cached back buffer (sdk/hb_lv_surface.c, HB_LV_BACKBUF) instead of the display framebuffer.
EXTRA_CFLAGS += -DHB_LV_BACKBUF
# AAP_BENCH=1: debug build that sweeps the page under several variants once after launch (B* lines in
# ./start trace). NEVER ship it on: the sweep takes ~12 s of frames, during which the app does not poll
# Bluetooth, so the AirPods connection stalls and the page looks dead (it did, 2026-11-25).
AAP_BENCH  ?= 0
ifeq ($(AAP_BENCH),1)
EXTRA_CFLAGS += -DAAP_UI_BENCH
endif
include ../../sdk/hb_app.mk
