#include "PeripheralMath.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    PcmMetrics m = {0};
    const int16_t samples[] = {1000,32767,-1000,32767,1000,-32768,-1000,-32768};
    PcmMetrics_AddStereoLeft(&m, samples, 4);
    assert(m.samples == 4 && m.peak == 1000 && m.clipped == 0 && m.sum == 0);
    assert(fabs(PcmMetrics_Rms(&m)-1000) < 0.01 && fabs(PcmMetrics_AcRms(&m)-1000) < 0.01);
    memset(&m, 0, sizeof(m));
    const int16_t dc[] = {-32768,0,-32768,0};
    PcmMetrics_AddStereoLeft(&m, dc, 2);
    assert(m.peak == 32768 && m.clipped == 2 && m.samples == 2);
    assert(PcmMetrics_AcRms(&m) == 0 && PcmMetrics_Rms(&m) == 32768);
    assert(PcmMetrics_Rms(NULL) == 0);
    uint8_t packet[15] = {0x06,6,12,0x48,0,1,0xab}; /* x100, y200 */
    uint16_t x[2],y[2]; uint8_t count;
    assert(Cst9217_Decode(packet,15,x,y,&count) && count==1 && x[0]==100 && y[0]==200);
    packet[5]=2; packet[7]=0x16; packet[8]=25; packet[9]=31; packet[10]=0x95;
    assert(Cst9217_Decode(packet,15,x,y,&count) && count==2 && x[1]==409 && y[1]==501);
    assert(!Cst9217_Decode(packet,11,x,y,&count));
    packet[10]=0xa5; assert(!Cst9217_Decode(packet,15,x,y,&count)); /* x410 invalid */
    packet[5]=0; assert(Cst9217_Decode(packet,15,x,y,&count) && count==0);
    packet[5]=3; assert(!Cst9217_Decode(packet,15,x,y,&count));
    packet[5]=1; packet[6]=0; assert(!Cst9217_Decode(packet,15,x,y,&count));
    assert(!Cst9217_Decode(NULL,15,x,y,&count));
    puts("peripheral math: PCM channel selection, RMS/DC/clipping and touch bounds PASS");
    return 0;
}
