#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum { SOUND_SPEAKER, SOUND_SPEAKER_STOP, SOUND_BUZZER, SOUND_BUZZER_STOP } SoundAction;
typedef struct {
    char id[17], boot_prefix[9];
    uint32_t deadline_ms, hz, duration_ms, volume_percent;
    SoundAction action;
} SoundCommand;
/* Strict, bounded command grammar. No hardware or clock access. */
bool SoundCommand_Parse(const char *line, SoundCommand *out);
