#include "ChargeCommand.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    char line[80]; ChargeCommandAck ack;
    assert(ChargeCommand_Format(line,sizeof(line),650,"0123456789abcdef"));
    assert(!strcmp(line,"BAT CURRENT 650 0123456789abcdef\n"));
    assert(!ChargeCommand_Format(line,sizeof(line),1000,"0123456789abcdef"));
    for (unsigned code=0;code<32;++code) {
        unsigned ma=50+((code&1)?100:0)+((code&2)?200:0)+((code&4)?500:0)+((code&8)?1200:0)+((code&16)?2300:0);
        assert(ChargeCommand_Format(line,sizeof(line),ma,"0123456789abcdef"));
    }
    assert(!ChargeCommand_Format(line,sizeof(line),650,"0123456789abcd\nX"));
    assert(!ChargeCommand_Format(line,5,650,"0123456789abcdef"));
    assert(ChargeCommand_ParseAck("BAT_CURRENT_ACK id=0123456789abcdef target_mA=650 status=APPLIED reason=OK applied_mA=650\r\n",&ack));
    assert(!ChargeCommand_ParseAck("BAT_CURRENT_ACK id=0123456789abcdef target_mA=650 status=APPLIED reason=OK applied_mA=850\r\n",&ack));
    assert(!ChargeCommand_ParseAck("BAT_CURRENT_ACK id=0123456789abcdef target_mA=650 status=PENDING reason=OK applied_mA=1850\r\n",&ack));
    assert(ChargeCommand_ParseAck("BAT_CURRENT_ACK id=0123456789abcdef target_mA=650 status=REJECTED reason=ASLEEP applied_mA=1850\r\n",&ack));
    assert(!ChargeCommand_ParseAck("BAT_CURRENT_ACK id=0123456789abcdef target_mA=650 status=APPLIED reason=OK applied_mA=650 EXTRA",&ack));
    puts("Charge command validation tests passed"); return 0;
}
