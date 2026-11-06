#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Single mailbox. Callers hold the audio state lock around every operation. */
typedef struct {
    unsigned hz, duration_ms, volume;
    uint32_t generation;
    char id[17];
} AudioToneRequest;
typedef struct {
    AudioToneRequest request;
    uint32_t generation;
    bool busy, pending, stop_pending;
} AudioToneControl;

static inline bool AudioTone_ValidId(const char *id)
{
    if (!id) return true; /* Legacy local caller has no external request ID. */
    if (strlen(id) != 16) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    return true;
}
static inline bool AudioTone_ValidArgs(unsigned hz, unsigned ms, unsigned volume)
{
    return hz >= 100 && hz <= 4000 && ms >= 20 && ms <= 1000 &&
        volume >= 1 && volume <= 20;
}
static inline bool AudioTone_Request(AudioToneControl *control, unsigned hz,
                                    unsigned ms, unsigned volume, const char *id)
{
    if (control->busy || !AudioTone_ValidArgs(hz, ms, volume) || !AudioTone_ValidId(id))
        return false;
    control->request = (AudioToneRequest) {
        .hz = hz, .duration_ms = ms, .volume = volume,
        .generation = ++control->generation,
    };
    if (id) memcpy(control->request.id, id, 17);
    control->busy = control->pending = true;
    control->stop_pending = false;
    return true;
}
static inline bool AudioTone_Stop(AudioToneControl *control, const char *id)
{
    if (!AudioTone_ValidId(id)) return false;
    control->request = (AudioToneRequest) {.generation = ++control->generation};
    if (id) memcpy(control->request.id, id, 17);
    control->busy = control->stop_pending = true;
    control->pending = false;
    return true;
}
static inline bool AudioTone_TakeStop(AudioToneControl *control, uint32_t *generation)
{
    if (!control->stop_pending) return false;
    *generation = control->generation;
    control->stop_pending = false;
    return true;
}
static inline bool AudioTone_Take(AudioToneControl *control, AudioToneRequest *request)
{
    if (!control->pending || control->stop_pending) return false;
    *request = control->request;
    control->pending = false;
    return true;
}
static inline bool AudioTone_Current(const AudioToneControl *control, uint32_t generation)
{
    return control->generation == generation;
}
static inline bool AudioTone_Complete(AudioToneControl *control, uint32_t generation)
{
    if (!AudioTone_Current(control, generation)) return false;
    control->busy = control->pending = control->stop_pending = false;
    return true;
}
