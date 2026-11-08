#include "DisplayLayout.h"
#include "DisplaySyncLogic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(DisplayLayout_WindowValid(0,0,410,502));
    assert(!DisplayLayout_WindowValid(0,1,410,16));
    assert(!DisplayLayout_WindowValid(1,0,408,16));
    assert(!DisplayLayout_WindowValid(0,0,410,15));
    assert(!DisplayLayout_WindowValid(0,0,409,16));
    for(unsigned width=2;width<=410;width+=2) {
        unsigned rows=DisplayLayout_DmaRows(width);
        assert(rows && !(rows&1U) && width*rows<=DISPLAY_DMA_MAX_PIXELS);
    }
    unsigned char seen[DISPLAY_TEXT_WIDTH * DISPLAY_TEXT_HEIGHT];
    memset(seen, 0, sizeof(seen));
    for (unsigned x = 0; x < DISPLAY_TEXT_WIDTH; ++x) {
        for (unsigned y = 0; y < DISPLAY_TEXT_HEIGHT; ++y) {
            unsigned offset = DisplayLayout_PixelOffset(x, y);
            unsigned previous_offset = (x * DISPLAY_TEXT_HEIGHT + DISPLAY_TEXT_HEIGHT - 1U - y) * 2U;
            assert(offset + previous_offset == sizeof(seen) * 2U - 2U);
            assert(offset % 2U == 0 && offset / 2U < sizeof(seen));
            assert(!seen[offset / 2U]);
            seen[offset / 2U] = 1;
        }
    }
    for (unsigned i = 0; i < sizeof(seen); ++i) assert(seen[i]);
    assert(DisplayLayout_PixelOffset(0, 0) == 431U * 24U * 2U);
    assert(DisplayLayout_PixelOffset(0, 23) == sizeof(seen) * 2U - 2U);
    assert(DisplayLayout_PixelOffset(431, 0) == 0U);
    assert(DisplayLayout_PixelOffset(432, 0) == UINT32_MAX);
    assert(DisplayLayout_PixelOffset(0, 24) == UINT32_MAX);
    for (unsigned px = 0; px < DISPLAY_PANEL_WIDTH; ++px) {
        for (unsigned py = 0; py < DISPLAY_PANEL_HEIGHT; ++py) {
            uint16_t x = (uint16_t)px, y = (uint16_t)py;
            assert(DisplayLayout_Touch(&x, &y));
            assert(x < DISPLAY_UI_WIDTH && y < DISPLAY_UI_HEIGHT);
            assert(DISPLAY_PANEL_HEIGHT - 1U - x == py && y == px);
        }
    }
    uint16_t x = 410, y = 0;
    assert(!DisplayLayout_Touch(&x, &y) && x == 410 && y == 0);
    x = 0; y = 502;
    assert(!DisplayLayout_Touch(&x, &y));
    assert(!DisplayLayout_Touch(NULL, &y));
    const unsigned rows[] = {32,76,120,164,208,252,296,350};
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        assert(rows[i] + DISPLAY_TEXT_HEIGHT <= DISPLAY_UI_HEIGHT);
        if (i) assert(rows[i - 1] + DISPLAY_TEXT_HEIGHT < rows[i]);
    }
    assert(DISPLAY_TEXT_X + DISPLAY_TEXT_WIDTH <= DISPLAY_UI_WIDTH);
    unsigned max_rows = DisplayLayout_DmaRows(DISPLAY_TEXT_HEIGHT);
    assert(max_rows * DISPLAY_TEXT_HEIGHT * 2U <= 16U * 1024U);
    uint32_t budget = DisplaySync_TransferBudgetUs(DISPLAY_TEXT_HEIGHT,
        DISPLAY_TEXT_WIDTH, 40000000, max_rows);
    assert(budget == 1195U);
    assert(DISPLAY_TEXT_NATIVE_Y == 54U);
    assert(DisplaySync_Check(1000000, 1000000, 16733, 3, DISPLAY_TEXT_NATIVE_Y,
        DISPLAY_PANEL_HEIGHT, budget, NULL) == DISPLAY_SYNC_READY);
    puts("landscape: full pixel coverage, counterclockwise orientation, touch inverse, bounds and TE/DMA budget PASS");
    return 0;
}
