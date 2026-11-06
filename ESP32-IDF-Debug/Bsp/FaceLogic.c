#include "FaceLogic.h"
#include <string.h>

void FaceLogic_Init(FaceLogic *s, uint32_t now)
{
    memset(s, 0, sizeof(*s));
    s->last_tap = now - 300U;
}
void FaceLogic_Screen(FaceLogic *s, uint32_t now, bool valid, bool down)
{
    if (!valid) { s->screen_down = false; s->screen_long = false; return; }
    s->screen_seen = now;
    if (down && !s->screen_down) {
        s->screen_at = now; s->screen_long = false;
    } else if (!down && s->screen_down) {
        uint32_t held = now - s->screen_at;
        if (!s->screen_long && held >= 30U && held < 1000U && now - s->last_tap >= 250U) {
            s->last_tap = now; ++s->taps;
            if (!s->face_page) s->face_page = true;
            else s->selected = (s->selected + 1U) % 4U;
        }
    }
    s->screen_down = down;
}
void FaceLogic_Head(FaceLogic *s, uint32_t now, uint32_t source_ms, bool down, uint32_t presses)
{
    if (s->head_known && source_ms == s->head_source) return; /* Cached UART is not a new sample. */
    bool reset = !s->head_known || (int32_t)(source_ms - s->head_source) < 0;
    uint32_t delta = presses - s->head_count;
    bool event = !reset && ((delta > 0U && delta < 0x80000000UL) || (down && !s->head_down));
    s->head_known = true; s->head_source = source_ms; s->head_count = presses;
    s->head_down = down; s->head_at = now;
    if (event) {
        ++s->head_events; s->pet_active = true; s->pet_at = now; s->face_page = true;
    }
}
void FaceLogic_Update(FaceLogic *s, uint32_t now)
{
    if (s->screen_down && now - s->screen_seen > 1500U) {
        s->screen_down = false; s->screen_long = false;
    }
    if (s->screen_down && !s->screen_long && now - s->screen_at >= 1200U) {
        s->face_page = false; s->screen_long = true;
    }
    if (s->head_known && s->head_down && now - s->head_at < 1200U) {
        s->pet_active = true; s->pet_at = now;
    }
}
FaceMood FaceLogic_Mood(const FaceLogic *s, uint32_t now)
{
    return s->pet_active && now - s->pet_at < 4000U ? FACE_COMFORT : (FaceMood)s->selected;
}
const char *FaceLogic_Name(FaceMood mood)
{
    static const char *names[] = {"CALM", "CURIOUS", "JOY", "SLEEPY", "COMFORT"};
    return (unsigned)mood <= FACE_COMFORT ? names[mood] : "CALM";
}
