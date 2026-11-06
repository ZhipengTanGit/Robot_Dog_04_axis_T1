#include "AudioToneControl.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    AudioToneControl control;
    memset(&control, 0, sizeof(control));
    AudioToneRequest request;
    uint32_t stop_generation;
    const char *play_id = "0123456789abcdef", *stop_id = "fedcba9876543210";
    assert(AudioTone_ValidArgs(100, 20, 1));
    assert(AudioTone_ValidArgs(4000, 1000, 20));
    assert(!AudioTone_ValidArgs(99, 20, 1));
    assert(!AudioTone_ValidArgs(4001, 20, 1));
    assert(!AudioTone_ValidArgs(100, 19, 1));
    assert(!AudioTone_ValidArgs(100, 1001, 1));
    assert(!AudioTone_ValidArgs(100, 20, 0));
    assert(!AudioTone_ValidArgs(100, 20, 21));
    assert(!AudioTone_Request(&control, 1000, 500, 5, "bad"));
    assert(!control.busy && !control.pending);
    assert(AudioTone_Request(&control, 1000, 500, 5, play_id));
    assert(!AudioTone_Stop(&control, "0123456789abcdeF"));
    assert(!strcmp(control.request.id, play_id) && control.pending);
    assert(!AudioTone_Request(&control, 2000, 500, 5, stop_id));
    assert(AudioTone_Take(&control, &request));
    assert(!strcmp(request.id, play_id) && request.hz == 1000);
    assert(!AudioTone_Take(&control, &request));
    assert(!AudioTone_Request(&control, 2000, 500, 5, stop_id));
    assert(AudioTone_Stop(&control, stop_id));
    assert(!AudioTone_Current(&control, request.generation));
    assert(!AudioTone_Complete(&control, request.generation));
    assert(control.busy && control.stop_pending);
    assert(!strcmp(control.request.id, stop_id));
    assert(AudioTone_TakeStop(&control, &stop_generation));
    uint32_t old_stop_generation = stop_generation;
    assert(AudioTone_Stop(&control, play_id));
    assert(!AudioTone_Complete(&control, old_stop_generation));
    assert(control.busy && control.stop_pending);
    assert(AudioTone_TakeStop(&control, &stop_generation));
    assert(AudioTone_Complete(&control, stop_generation));
    assert(!control.busy);
    assert(AudioTone_Request(&control, 1000, 500, 5, play_id));
    assert(AudioTone_Stop(&control, stop_id)); /* Stop cancels queued playback. */
    assert(!AudioTone_Take(&control, &request));
    assert(AudioTone_TakeStop(&control, &stop_generation));
    assert(AudioTone_Complete(&control, stop_generation));
    control.generation = UINT32_MAX;
    assert(AudioTone_Request(&control, 1000, 500, 5, NULL));
    assert(AudioTone_Take(&control, &request));
    assert(request.generation == 0 && request.id[0] == '\0');
    assert(AudioTone_Complete(&control, request.generation));
    puts("audio tone: bounds, single mailbox, stop precedence, ID isolation and wrap PASS");
    return 0;
}
