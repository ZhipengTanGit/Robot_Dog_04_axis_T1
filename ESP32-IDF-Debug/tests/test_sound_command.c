#include "SoundCommandLogic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    SoundCommand c;
    assert(SoundCommand_Parse("ESP SOUND 0123456789abcdef 4294967295 abcdef01 SPEAKER 100 20 1",&c));
    assert(c.deadline_ms==UINT32_MAX && c.hz==100 && c.duration_ms==20 && c.action==SOUND_SPEAKER);
    assert(SoundCommand_Parse("ESP SOUND 0123456789abcdef 0 abcdef01 SPEAKER 4000 1000 20",&c));
    assert(SoundCommand_Parse("ESP SOUND 0123456789abcdef 50 abcdef01 BUZZER 200",&c));
    assert(c.action==SOUND_BUZZER && c.duration_ms==200);
    assert(SoundCommand_Parse("ESP SOUND 0123456789abcdef 50 abcdef01 BUZZER_STOP",&c));
    assert(SoundCommand_Parse("ESP SOUND 0123456789abcdef 50 abcdef01 SPEAKER_STOP",&c));
    const char *bad[]={"", "ESP SOUND ", "ESP SOUND 0123456789abcdef 4294967296 abcdef01 BUZZER 1",
        "ESP SOUND 0123456789abcdef 50 abcdef01 BUZZER 0", "ESP SOUND 0123456789abcdef 50 abcdef01 BUZZER 1001",
        "ESP SOUND 0123456789abcdef 50 abcdef01 SPEAKER 1000 100 21", "ESP SOUND 0123456789abcdef 50 abcdef01 SPEAKER 99 100 1",
        "ESP SOUND 0123456789abcdef 50 abcdef01 SPEAKER_STOP extra", "ESP SOUND 0123456789abcdef 50 abcdef01 BUZZER -20",
        "ESP SOUND 0123456789abcdeF 50 abcdef01 BUZZER 100", "ESP SOUND 0123456789abcdef 50 abcdef01 BUZZER 020"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) assert(!SoundCommand_Parse(bad[i],&c));
    /* Every truncated prefix of a command must be rejected without overread. */
    char line[]="ESP SOUND 0123456789abcdef 123456 abcdef01 SPEAKER 1000 500 10";
    for(size_t i=0;i<strlen(line)-2;++i) { char copy[sizeof(line)];memcpy(copy,line,i);copy[i]=0;assert(!SoundCommand_Parse(copy,&c)); }
    puts("sound command parser PASS");
    return 0;
}
