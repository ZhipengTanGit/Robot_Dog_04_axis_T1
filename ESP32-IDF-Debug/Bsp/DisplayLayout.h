#pragma once
#include <stdbool.h>
#include <stdint.h>

/* CO5300 keeps its native scan/address order. Rotate the UI in software. */
#define DISPLAY_PANEL_WIDTH 410U
#define DISPLAY_PANEL_HEIGHT 502U
#define DISPLAY_PANEL_X_OFFSET 22U
#define DISPLAY_UI_WIDTH DISPLAY_PANEL_HEIGHT
#define DISPLAY_UI_HEIGHT DISPLAY_PANEL_WIDTH
#define DISPLAY_UI_ROTATION_DEG 270U
#define DISPLAY_TEXT_X 16U
#define DISPLAY_TEXT_COLUMNS 24U
#define DISPLAY_TEXT_WIDTH (DISPLAY_TEXT_COLUMNS * 18U)
#define DISPLAY_TEXT_HEIGHT 24U
#define DISPLAY_TEXT_NATIVE_Y (DISPLAY_PANEL_HEIGHT - DISPLAY_TEXT_X - DISPLAY_TEXT_WIDTH)
#define DISPLAY_DMA_MAX_PIXELS (DISPLAY_PANEL_WIDTH * 16U)
static inline bool DisplayLayout_WindowValid(unsigned x,unsigned y,unsigned width,unsigned height)
{
    return width && height && x<DISPLAY_PANEL_WIDTH && y<DISPLAY_PANEL_HEIGHT &&
        width<=DISPLAY_PANEL_WIDTH-x && height<=DISPLAY_PANEL_HEIGHT-y &&
        (((x+DISPLAY_PANEL_X_OFFSET)|y|width|height)&1U)==0;
}
static inline unsigned DisplayLayout_DmaRows(unsigned width)
{
    return width && !(width&1U) && width<=DISPLAY_PANEL_WIDTH ? (DISPLAY_DMA_MAX_PIXELS/width)&~1U : 0U;
}

static inline unsigned DisplayLayout_PixelOffset(unsigned x, unsigned y)
{
    /* Counterclockwise: native x = UI y; native y = 501 - UI x.
     * This buffer covers only the text rectangle, in native row order. */
    if (x >= DISPLAY_TEXT_WIDTH || y >= DISPLAY_TEXT_HEIGHT) return UINT32_MAX;
    return ((DISPLAY_TEXT_WIDTH - 1U - x) * DISPLAY_TEXT_HEIGHT + y) * 2U;
}

static inline bool DisplayLayout_Touch(uint16_t *x, uint16_t *y)
{
    if (!x || !y || *x >= DISPLAY_PANEL_WIDTH || *y >= DISPLAY_PANEL_HEIGHT) return false;
    uint16_t native_x = *x;
    *x = (uint16_t)(DISPLAY_PANEL_HEIGHT - 1U - *y);
    *y = native_x;
    return true;
}
