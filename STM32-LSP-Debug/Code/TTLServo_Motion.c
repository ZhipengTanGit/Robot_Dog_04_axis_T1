#include "TTLServo_Motion.h"
#include "Servo_Config.h"
#include <string.h>
enum { IDLE,GOAL,GOAL_CHECK,ENABLE,ON_CHECK,OFF,OFF_CHECK };
typedef struct {
    uint16_t goal,sent_goal,speed,sent_speed;
    uint32_t generation,sent_generation,retry_at;
    uint8_t want,touched,on,stage,id,off_verified;
} Control;
static Control controls[4];
static uint8_t teaching;
static size_t packet(uint8_t id,uint8_t instruction,uint8_t address,const uint8_t *data,size_t count,uint8_t *p)
{
    size_t i;unsigned sum;
    p[0]=p[1]=255;p[2]=id;p[3]=(uint8_t)(count+3U);p[4]=instruction;p[5]=address;
    for(i=0;i<count;++i)p[6+i]=data[i];
    sum=0;for(i=2;i<count+6U;++i)sum+=p[i];p[count+6U]=(uint8_t)(255U-(sum&255U));
    return count+7U;
}
static int readback(const uint8_t *p,size_t size,const uint8_t *expected,size_t count)
{
    size_t i,j;unsigned sum;
    for(i=0;i+count+6U<=size;++i){
        if(p[i]!=255||p[i+1]!=255||p[i+3]!=count+2U||p[i+4]!=0)continue;
        sum=0;for(j=2;j<count+6U;++j)sum+=p[i+j];
        if((sum&255U)==255U&&!memcmp(p+i+5U,expected,count))return 1;
    }return 0;
}
void TTLServo_MotionInit(void){memset(controls,0,sizeof(controls));teaching=0;}
uint8_t TTLServo_IsTeaching(void){return teaching;}
void TTLServo_EndTeach(void){unsigned i;teaching=0;for(i=0;i<4;++i)if(controls[i].off_verified)controls[i].retry_at=0;}
void TTLServo_BeginTeach(void)
{
    unsigned i;teaching=1;
    for(i=0;i<4;++i){Control *c=&controls[i];c->want=0;c->touched=1;
        c->off_verified=0;c->stage=OFF;c->retry_at=0;++c->generation;}
}
int TTLServo_SetTarget(uint8_t ch,uint16_t pulse,uint32_t now)
{
    return TTLServo_SetTargetSpeed(ch,pulse,100U,now);
}
int TTLServo_SetTargetSpeed(uint8_t ch,uint16_t pulse,uint16_t speed,uint32_t now)
{
    TTLServo_Status s;Control *c;
    if(teaching||ch>=4||pulse<500||pulse>2500||speed<1U||speed>1023U)return -1;
    TTLServo_GetStatus(ch,now,&s);
    if(!s.valid||s.status||s.baud!=115200UL)return -1;
    c=&controls[ch];
    {uint16_t goal=(uint16_t)(((uint32_t)(pulse-500U)*SERVO_TTL_POSITION_MAX+1000UL)/2000UL);
    if(c->want&&c->goal==goal&&c->speed==speed)return 0;c->goal=goal;c->speed=speed;}
    c->want=c->touched=1;c->stage=GOAL;++c->generation;return 0;
}
uint8_t TTLServo_TorqueState(uint8_t ch)
{
    Control *c=&controls[ch];
    return c->touched ? (!c->want?3U:c->stage==IDLE&&c->on?2U:1U) : c->off_verified?4U:0U;
}
int TTLServo_SetPose(const uint16_t pulses[4],uint32_t now)
{
    unsigned i;TTLServo_Status s;
    for(i=0;i<4;++i){TTLServo_GetStatus((uint8_t)i,now,&s);
        if(pulses[i]<500U||pulses[i]>2500U||!s.valid||s.status||s.baud!=115200UL)return -1;}
    TTLServo_EndTeach();
    /* All calls use the same snapshot time, with no task or interrupt writer
     * to the cached feedback between validation and committing the targets. */
    for(i=0;i<4;++i)(void)TTLServo_SetTarget((uint8_t)i,pulses[i],now);
    return 0;
}
void TTLServo_StopAll(void)
{
    unsigned i;for(i=0;i<4;++i){Control *c=&controls[i];c->want=0;++c->generation;if(c->touched)c->stage=OFF;}
}
uint8_t TTLServo_ActiveMask(void)
{
    unsigned i;uint8_t mask=0;for(i=0;i<4;++i)if(controls[i].touched)mask|=(uint8_t)(1U<<i);return mask;
}
int TTLServo_MotionNext(uint8_t ch,uint8_t id,uint8_t *p,size_t *size,uint32_t now)
{
    Control *c=&controls[ch];uint8_t data[4];int op=c->stage;
    if(teaching&&op==IDLE&&c->off_verified&&(int32_t)(now-c->retry_at)>=0){c->stage=OFF_CHECK;op=OFF_CHECK;}
    if(!op||(int32_t)(now-c->retry_at)<0)return 0;
    c->sent_generation=c->generation;c->sent_goal=c->goal;c->sent_speed=c->speed;c->id=id;
    if(op==GOAL){data[0]=(uint8_t)c->goal;data[1]=(uint8_t)(c->goal>>8);data[2]=(uint8_t)c->speed;data[3]=(uint8_t)(c->speed>>8);*size=packet(id,3,0x1E,data,4,p);}
    else if(op==ENABLE||op==OFF){data[0]=(uint8_t)(op==ENABLE);*size=packet(id,3,0x18,data,1,p);}
    else {p[0]=p[1]=255;p[2]=id;p[3]=4;p[4]=2;p[5]=(uint8_t)(op==GOAL_CHECK?0x1E:0x18);p[6]=(uint8_t)(op==GOAL_CHECK?4:1);p[7]=(uint8_t)(255U-(uint8_t)(id+4U+2U+p[5]+p[6]));*size=8;}
    return op;
}
void TTLServo_MotionComplete(uint8_t ch,int op,const uint8_t *reply,size_t size,int result,uint32_t now)
{
    Control *c=&controls[ch];uint8_t expected[4];
    TTLServo_Status ack;
    if(c->generation!=c->sent_generation){c->stage=c->want?GOAL:OFF;return;}
    if(!result&&(op==GOAL||op==ENABLE||op==OFF)&&size&&TTLServo_ParsePing(reply,size,c->id,&ack)==0&&ack.status)result=-1;
    if(result){c->want=0;c->touched=1;c->off_verified=0;c->stage=OFF;c->retry_at=now+500U;return;}
    if(op==GOAL){c->stage=GOAL_CHECK;return;}
    if(op==ENABLE){c->stage=ON_CHECK;return;}
    if(op==OFF){c->stage=OFF_CHECK;return;}
    expected[0]=(uint8_t)(op==ON_CHECK);
    if(op==GOAL_CHECK){expected[0]=(uint8_t)c->sent_goal;expected[1]=(uint8_t)(c->sent_goal>>8);expected[2]=(uint8_t)c->sent_speed;expected[3]=(uint8_t)(c->sent_speed>>8);}
    if(size<3U||reply[2]!=c->id||!readback(reply,size,expected,op==GOAL_CHECK?4U:1U)){c->want=0;c->touched=1;c->off_verified=0;c->stage=OFF;c->retry_at=now+500U;return;}
    if(op==GOAL_CHECK)c->stage=c->want?ENABLE:OFF;
    else if(op==ON_CHECK){c->on=1;c->stage=IDLE;}
    else {c->on=c->touched=0;c->stage=IDLE;c->off_verified=1;c->retry_at=teaching?now+200U:0;}
}
