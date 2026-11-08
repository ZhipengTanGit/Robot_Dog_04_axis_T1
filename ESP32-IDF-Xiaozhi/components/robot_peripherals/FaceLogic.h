#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { FACE_CALM, FACE_CURIOUS, FACE_JOY, FACE_SLEEPY, FACE_COMFORT } FaceMood;
typedef struct {
    bool face_page, screen_down, screen_long, head_known, head_down, pet_active;
    uint32_t screen_at, screen_seen, last_tap, pet_at, head_at, head_source, head_count;
    uint32_t taps, head_events;
    unsigned selected;
} FaceLogic;
void FaceLogic_Init(FaceLogic *s, uint32_t now);
void FaceLogic_Screen(FaceLogic *s, uint32_t now, bool valid, bool down);
void FaceLogic_Head(FaceLogic *s, uint32_t now, uint32_t source_ms, bool down, uint32_t presses);
void FaceLogic_Update(FaceLogic *s, uint32_t now);
FaceMood FaceLogic_Mood(const FaceLogic *s, uint32_t now);
const char *FaceLogic_Name(FaceMood mood);
