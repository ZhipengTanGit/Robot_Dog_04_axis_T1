#pragma once
#include <stdint.h>
/* OV5640 BRMATRX is a 5x5 grid: blue in the high nibble, red in low. */
static inline uint8_t LensEdge_Value(uint8_t original,unsigned column,unsigned profile)
{
    int r=original&15,b=original>>4;
    if(column==4U) {
        if(profile==1U)++r;
        if(profile==3U){r+=5;b+=3;}
    } else if(column==3U && profile==3U){r+=3;b+=2;}
    if(profile==2U && column==0U){r+=5;b+=3;}
    else if(profile==2U && column==1U){r+=3;b+=2;}
    if(r<0)r=0;
    if(r>15)r=15;
    if(b>15)b=15;
    return (uint8_t)((b<<4)|r);
}
