
#include "TTLServo_Motion.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t regs[64], valid=1;
static uint32_t now=1;
void TTLServo_GetStatus(uint8_t ch,uint32_t tick,TTLServo_Status *s){(void)ch;(void)tick;memset(s,0,sizeof(*s));s->valid=valid;s->baud=115200;}
static int step(int corrupt)
{
    uint8_t p[16],r[24];size_t size,n,i;unsigned sum;int op;
    op=TTLServo_MotionNext(0,1,p,&size,now++);if(!op)return 0;
    assert(p[0]==255&&p[1]==255&&p[2]==1);
    sum=0;for(i=2;i<size;++i)sum+=p[i];assert((sum&255)==255);
    r[0]=r[1]=255;r[2]=1;r[4]=0;
    if(p[4]==3){assert(p[5]==0x18||p[5]==0x1E);memcpy(regs+p[5],p+6,size-7);r[3]=2;n=6;}
    else {assert(p[4]==2);r[3]=(uint8_t)(p[6]+2);memcpy(r+5,regs+p[5],p[6]);n=p[6]+6;}
    if(corrupt)r[2]=2;
    sum=0;for(i=2;i<n-1;++i)sum+=r[i];r[n-1]=(uint8_t)(255-(sum&255));
    TTLServo_MotionComplete(0,op,r,n,0,now);return op;
}
int main(void)
{
    uint8_t p[16];size_t n;int op;unsigned i;
    TTLServo_MotionInit();assert(!TTLServo_ActiveMask()&&!step(0)&&!regs[0x18]);
    valid=0;assert(TTLServo_SetTarget(0,1500,now)<0);valid=1;
    assert(TTLServo_SetTarget(0,499,now)<0);
    assert(TTLServo_SetTarget(0,1500,now)==0);
    assert(step(0)==1&&!regs[0x18]);
    assert(step(0)==2&&!regs[0x18]);
    assert(regs[0x1E]==0&&regs[0x1F]==2&&regs[0x20]==100);
    assert(step(0)==3&&regs[0x18]==1);assert(step(0)==4);assert(!step(0));
    TTLServo_StopAll();assert(TTLServo_ActiveMask()==1);assert(step(0)==5&&!regs[0x18]);
    assert(TTLServo_ActiveMask()==1);assert(step(0)==6&&!TTLServo_ActiveMask());
    /* Speed is committed with the goal and verified before enabling torque. */
    assert(TTLServo_SetTargetSpeed(0,1500,0,now)<0);
    assert(TTLServo_SetTargetSpeed(0,1500,1024,now)<0);
    assert(TTLServo_SetTargetSpeed(0,1500,300,now)==0);
    assert(step(0)==1);assert(regs[0x20]==44&&regs[0x21]==1);
    assert(step(0)==2);assert(step(0)==3);assert(step(0)==4);
    assert(TTLServo_SetTargetSpeed(0,1500,50,now)==0);
    assert(step(0)==1);assert(regs[0x20]==50&&regs[0x21]==0);
    assert(step(0)==2);assert(step(0)==3);assert(step(0)==4);
    TTLServo_StopAll();assert(step(0)==5);assert(step(0)==6);
    /* A STOP during an in-flight goal prevents any later torque enable. */
    assert(TTLServo_SetTarget(0,1600,now)==0);
    op=TTLServo_MotionNext(0,1,p,&n,now++);assert(op==1);TTLServo_StopAll();
    TTLServo_MotionComplete(0,op,0,0,0,now);assert(step(0)==5);assert(step(0)==6);assert(!step(0));
    /* A foreign-ID readback cannot authorize enable, or confirm torque OFF. */
    assert(TTLServo_SetTarget(0,1700,now)==0);assert(step(0)==1);assert(step(1)==2);
    now+=501;assert(step(0)==5);assert(step(1)==6);assert(TTLServo_ActiveMask());
    now+=501;assert(step(0)==5);assert(step(0)==6);assert(!TTLServo_ActiveMask());
    for(i=0;i<4;++i){assert(TTLServo_SetTarget(0,1500,now)==0);step(0);}
    TTLServo_StopAll();step(0);step(0);assert(!regs[0x18]);
    TTLServo_MotionInit();TTLServo_BeginTeach();assert(TTLServo_IsTeaching());
    assert(TTLServo_ActiveMask()==15); /* Includes channels never commanded. */
    for(i=0;i<4;++i)assert(TTLServo_SetTarget((uint8_t)i,1500,now)<0);
    assert(step(0)==5&&step(0)==6);assert(TTLServo_TorqueState(0)==4);
    assert(!step(0));now+=201;assert(step(1)==6);assert(TTLServo_TorqueState(0)==3);
    now+=501;assert(step(0)==5&&step(0)==6);assert(TTLServo_TorqueState(0)==4);
    TTLServo_EndTeach();assert(!TTLServo_IsTeaching());assert(TTLServo_SetTarget(0,1500,now)==0);assert(step(0)==1);
    puts("TTL motion tests passed including teaching release of untouched channels, torque lock, periodic OFF verification and corrupt feedback rejection.");return 0;
}
