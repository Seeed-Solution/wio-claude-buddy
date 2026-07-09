// The fake TFT_eSPI behind the shared buddy engine: renders the classic
// 6x8 GLCD font (same table the Wio's Seeed_Arduino_LCD uses, so the pet is
// pixel-identical) into an LVGL true-color canvas. The engine calls exactly
// fillRect / setTextColor / setCursor / setTextSize / print — nothing else.
#include "buddy_canvas.h"

#include <string.h>
#include "esp_heap_caps.h"

#include "wio_platform.h"   // the TFT_eSPI class declaration (shim)
#include "glcd_font.h"

TFT_eSPI spr;   // the engine's render target (extern in the shim)

static lv_color_t* sBuf = nullptr;
static lv_obj_t*   sCanvas = nullptr;

// cursor/text state (the subset TFT_eSPI tracks for print())
static int      sCurX = 0, sCurY = 0;
static uint8_t  sSize = 1;
static uint16_t sFg = 0xFFFF, sBg = 0x0000;

static inline lv_color_t c565(uint16_t c) {
  lv_color_t out;
  out.full = c;          // LVGL is RGB565 here (LV_COLOR_DEPTH 16, no swap)
  return out;
}

static inline void plot(int x, int y, lv_color_t c) {
  if (x < 0 || y < 0 || x >= BUDDY_CANVAS_PX_W || y >= BUDDY_CANVAS_PX_H) return;
  sBuf[y * BUDDY_CANVAS_PX_W + x] = c;
}

void TFT_eSPI::fillRect(int x, int y, int w, int h, uint16_t color565) {
  if (!sBuf) return;
  lv_color_t c = c565(color565);
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      plot(xx, yy, c);
}

void TFT_eSPI::setTextColor(uint16_t fg565, uint16_t bg565) { sFg = fg565; sBg = bg565; }
void TFT_eSPI::setCursor(int x, int y) { sCurX = x; sCurY = y; }
void TFT_eSPI::setTextSize(uint8_t s)  { sSize = s ? s : 1; }

void TFT_eSPI::print(char ch) {
  if (!sBuf) return;
  unsigned char c = (unsigned char)ch;
  if (c > 0x7F) c = '?';
  lv_color_t fg = c565(sFg);
  // 5 font columns + 1 blank spacing column = the classic 6px advance
  for (int col = 0; col < 5; col++) {
    unsigned char bits = glcd_font[(int)c * 5 + col];
    for (int row = 0; row < 8; row++) {
      if (bits & (1 << row)) {
        for (int dy = 0; dy < sSize; dy++)
          for (int dx = 0; dx < sSize; dx++)
            plot(sCurX + col * sSize + dx, sCurY + row * sSize + dy, fg);
      }
      // transparent background: the engine clears its strip each frame
    }
  }
  sCurX += 6 * sSize;
}

void TFT_eSPI::print(const char* s) {
  while (*s) print(*s++);
}

lv_obj_t* buddyCanvasCreate(lv_obj_t* parent) {
  size_t bytes = BUDDY_CANVAS_PX_W * BUDDY_CANVAS_PX_H * sizeof(lv_color_t);
  sBuf = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!sBuf) sBuf = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
  memset(sBuf, 0, bytes);
  sCanvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(sCanvas, sBuf, BUDDY_CANVAS_PX_W, BUDDY_CANVAS_PX_H,
                       LV_IMG_CF_TRUE_COLOR);
  // Integer upscale keeps the ASCII art crisp (240 = pivot-centered 2x...
  // zoom is x256; 512 = 2x -> 300x380 on the 480x480 panel).
  lv_img_set_antialias(sCanvas, false);
  lv_img_set_zoom(sCanvas, 512);
  return sCanvas;
}

void buddyCanvasFlush(void) {
  if (sCanvas) lv_obj_invalidate(sCanvas);
}
