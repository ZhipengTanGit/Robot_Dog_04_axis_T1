#include "ChargeCommand.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
bool ChargeCommand_Allowed(unsigned ma)
{
    for (unsigned code=0; code<32; ++code) {
        unsigned value=50 + ((code&1)?100:0) + ((code&2)?200:0) +
            ((code&4)?500:0) + ((code&8)?1200:0) + ((code&16)?2300:0);
        if (ma==value) return true;
    }
    return false;
}
bool ChargeCommand_IdValid(const char *id)
{
    if (!id || strlen(id) != 16) return false;
    for (unsigned i=0; i<16; ++i)
        if (!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f'))) return false;
    return true;
}
size_t ChargeCommand_Format(char *buffer, size_t size, unsigned ma, const char *id)
{
    if (!buffer || !size || !ChargeCommand_Allowed(ma) || !ChargeCommand_IdValid(id)) return 0;
    int n=snprintf(buffer,size,"BAT CURRENT %u %s\n",ma,id);
    return n>0 && (size_t)n<size ? (size_t)n : 0;
}
bool ChargeCommand_ParseAck(const char *line, ChargeCommandAck *out)
{
    if (!line || !out || strlen(line)>240) return false;
    ChargeCommandAck a; int used=0;
    memset(&a,0,sizeof(a));
    if (sscanf(line,"BAT_CURRENT_ACK id=%32[A-Za-z0-9_-] target_mA=%u status=%11[A-Z] reason=%39[A-Z0-9_] applied_mA=%u%n",
               a.id,&a.target_ma,a.status,a.reason,&a.applied_ma,&used)!=5 || !used) return false;
    for (const char *p=line+used; *p; ++p) if (*p!='\r' && *p!='\n') return false;
    if (!ChargeCommand_IdValid(a.id) || !ChargeCommand_Allowed(a.target_ma) ||
        (strcmp(a.status,"APPLIED") && strcmp(a.status,"FAILED") && strcmp(a.status,"REJECTED"))) return false;
    if (!strcmp(a.status,"APPLIED") && a.applied_ma!=a.target_ma) return false;
    *out=a; return true;
}
