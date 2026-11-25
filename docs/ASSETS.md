# Artwork

The page shows a picture of the AirPods it is connected to (the buds and the case, or the headphones for AirPods
Max). These pictures are Apple's, so they are the one part of this repository that is not MIT-licensed:
see [NOTICE.md](../NOTICE.md).

## What is embedded

- `app/aap_ctl/aap_art.c` / `aap_art.h`: nine picture sets (AirPods 1, 2, 3, 4, Pro, Pro 2, Pro 3, Max, Max USB-C),
  each about 20 KB, RGB565, already composited on the page's black background and scaled to the size they have on
  the 240 x 432 screen. The nano's LVGL is built without ARGB8888 image support, which is why they are baked in
  this way.
- `app/aap_ctl/icon_art.png`: the AirPods Pro bud on the home-screen icon (NanoApps fits it inside the icon disc).

## Regenerating them

`tools/make_art.py` and `tools/make_icon.py` turn a folder of AirPods renders into those files. They need
**your own** copy of the renders (they are not in this repository), laid out like this:

```
ASSET_DIR/
  airpods-1/            left@3x.png  right@3x.png  case-closed@3x.png
  airpods-2/            same
  airpods-3/            same
  airpods-4/            same
  airpods-pro-1/        same
  airpods-pro-2/        same
  airpods-pro-3/        left-small@2x.png  right-small@2x.png  case-closed-charged@2x.png
  airpods-max/          headphones-space-gray@3x.png
  airpods-max-usb-c/    headphones-midnight@3x.png
```

```sh
python3 tools/make_art.py  ASSET_DIR      # -> app/aap_ctl/aap_art.c, aap_art.h   (needs Pillow)
python3 tools/make_icon.py ASSET_DIR      # -> app/aap_ctl/icon_art.png
```

`make_art.py` also finds the green LED that is baked into the case renders, paints it out and exports its position,
so the page can draw the LED in the colour the case really shows (green, amber, off).

## Building without Apple's pictures

```sh
./install.sh --no-apple-art        # or: make -C app/aap_ctl APPLE_ART=0 inside a NanoApps checkout
```

This swaps `aap_art.c` for `aap_art_none.c` (no pictures) and leaves out `icon_art.png`. The page then draws plain,
generic stand-ins for the buds and the case, the home-screen icon falls back to NanoApps' headphones glyph, and the
app is about 90 KB smaller. Battery, labels and controls are unaffected.

## Using your own artwork instead

Any set of RGB565 `lv_image_dsc_t` pictures works: provide `aap_art_get()` as declared in `aap_art.h` and the page
needs no other change (`NULL` for an id means "draw the stand-ins"). Keep the executable small: NanoApps apps
should stay well under about half a megabyte.

## Other generated files

| Files | Made by | Source |
|---|---|---|
| `aap_icons.c/.h` (the four noise-control icons) | `tools/make_icons.py` | drawn in the script, no outside assets |
| `aap_font_14/16/20.c` | `tools/make_fonts.sh` | Inter, SIL OFL 1.1 |
