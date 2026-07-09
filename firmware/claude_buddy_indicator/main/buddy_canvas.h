#pragma once
// LVGL canvas hosting the shared ASCII buddy engine's render target.
// buddy.cpp draws into `spr` (the fake TFT_eSPI from the wio_platform.h
// shim); this module owns the pixel buffer and exposes the canvas object.
#include "lvgl.h"

// Native render size — the Wio's left-pane geometry at the engine's 2x home
// scale (buddy.cpp clears (BUDDY_Y_BASE + 5*8 + 12) * 2 = 184 rows).
#define BUDDY_CANVAS_PX_W 150
#define BUDDY_CANVAS_PX_H 190

lv_obj_t* buddyCanvasCreate(lv_obj_t* parent);  // creates + returns the canvas
void      buddyCanvasFlush(void);               // invalidate after buddyTick()
