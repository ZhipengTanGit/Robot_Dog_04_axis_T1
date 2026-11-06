#include "SoundCommandLogic.h"
#include <string.h>
#include <limits.h>
static bool hex(const char **p, char *out, unsigned count)
{
    for (unsigned i=0;i<count;++i) {
        char c=*(*p)++;
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
        out[i]=c;
    }
    out[count]=0;
    return true;
}
static bool number(const char **p, uint32_t *value)
{
    const char *start=*p; uint32_t n=0;
    if (**p<'0'||**p>'9') return false;
    while (**p>='0'&&**p<='9') {
        unsigned d=(unsigned)(**p-'0');
        if (n>(UINT32_MAX-d)/10U) return false;
        n=n*10U+d; ++*p;
    }
    if (*p-start>1 && *start=='0') return false;
    *value=n; return true;
}
bool SoundCommand_Parse(const char *line, SoundCommand *out)
{
    if (!line||!out||strncmp(line,"ESP SOUND ",10)) return false;
    SoundCommand c; memset(&c,0,sizeof(c)); const char *p=line+10;
    if (strlen(p)<28 || !hex(&p,c.id,16)||*p++!=' '||!number(&p,&c.deadline_ms)||*p++!=' '||
        strlen(p)<9||!hex(&p,c.boot_prefix,8)||*p++!=' ') return false;
    if (!strcmp(p,"SPEAKER_STOP")) c.action=SOUND_SPEAKER_STOP;
    else if (!strcmp(p,"BUZZER_STOP")) c.action=SOUND_BUZZER_STOP;
    else if (!strncmp(p,"SPEAKER ",8)) {
        c.action=SOUND_SPEAKER; p+=8;
        if (!number(&p,&c.hz)||*p++!=' '||!number(&p,&c.duration_ms)||*p++!=' '||
            !number(&p,&c.volume_percent)||*p||c.hz<100||c.hz>4000||c.duration_ms<20||
            c.duration_ms>1000||c.volume_percent<1||c.volume_percent>20) return false;
    } else if (!strncmp(p,"BUZZER ",7)) {
        c.action=SOUND_BUZZER;p+=7;
        if (!number(&p,&c.duration_ms)||*p||c.duration_ms<20||c.duration_ms>1000) return false;
    } else return false;
    *out=c;return true;
}
