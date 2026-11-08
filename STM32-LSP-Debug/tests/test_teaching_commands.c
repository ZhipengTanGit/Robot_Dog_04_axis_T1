/* Share the UART, battery and GPIO host harness; execute only TTL cases. */
#define main peripheral_legacy_tests
#include "test_peripherals.c"
#undef main
#include "TTLServo_Motion.h"
static uint8_t offline=255;
static uint32_t feedback_baud=115200;
static uint8_t feedback_flags;
void TTLServo_Init(void){TTLServo_MotionInit();}
void TTLServo_Task(uint32_t tick_value){(void)tick_value;}
void TTLServo_ResetTiming(void){}
void TTLServo_PrintStatus(uint32_t tick_value){(void)tick_value;}
void TTLServo_GetStatus(uint8_t ch,uint32_t tick_value,TTLServo_Status *s)
{(void)tick_value;memset(s,0,sizeof(*s));s->valid=(uint8_t)(ch!=offline);s->baud=feedback_baud;s->status=feedback_flags;s->position=512U;}
static void settle_motion(void)
{
    static uint8_t registers[4][64];unsigned pass,ch,i,sum;uint8_t p[16],r[24];size_t size,n;int op;
    for(pass=0;pass<8U;++pass)for(ch=0;ch<4U;++ch){
        op=TTLServo_MotionNext((uint8_t)ch,1,p,&size,now);if(!op)continue;
        r[0]=r[1]=255;r[2]=1;r[4]=0;
        if(p[4]==3){memcpy(registers[ch]+p[5],p+6,size-7U);r[3]=2;n=6;}
        else{r[3]=(uint8_t)(p[6]+2U);memcpy(r+5,registers[ch]+p[5],p[6]);n=p[6]+6U;}
        sum=0;for(i=2;i<n-1U;++i)sum+=r[i];r[n-1U]=(uint8_t)(255U-(sum&255U));
        TTLServo_MotionComplete((uint8_t)ch,op,r,n,0,now);
    }
}
int main(void)
{
    uint8_t packet_bytes[16];size_t n;
    reset();tick(4000);
    feed("PERIPH TEACH 1 4000\n");assert(TTLServo_IsTeaching()&&TTLServo_ActiveMask()==15);
    feed("PERIPH LIVE 2 4000 0 1500\nPERIPH SERVO 0 1500\n");
    assert(strstr(ack_output,"cmd=LIVE id=2 ok=0 reason=TEACH_LOCK"));
    assert(strstr(ack_output,"cmd=SERVO ok=0 reason=TEACH_LOCK"));
    offline=3;feed("PERIPH POSE 3 4000 500 1000 1500 2500\n");
    assert(TTLServo_IsTeaching()&&state().servo_mode==SERVO_MODE_IDLE);
    offline=255;feed("PERIPH POSE 4 4000 500 1000 1500 2500\n");
    assert(!TTLServo_IsTeaching()&&state().servo_mode==SERVO_MODE_LIVE);
    assert(state().servo_us[0]==500&&state().servo_us[3]==2500);
    assert(TTLServo_MotionNext(0,1,packet_bytes,&n,now)==1);
    feed("PERIPH STOP 5\nPERIPH POSE 4 4000 500 1000 1500 2500\n");
    assert(state().servo_mode==SERVO_MODE_IDLE&&strstr(ack_output,"cmd=POSE id=4 ok=0 reason=OLD_ID"));
    feed("PERIPH TEACH 6 4000\nPERIPH STOP 7\n");assert(!TTLServo_IsTeaching());
    feed("PERIPH POSE 8 4000 500 1000 1500 2501\n");assert(state().servo_mode==SERVO_MODE_IDLE);
    tick(7000);feed("PERIPH POSE 9 4000 500 1000 1500 2500\n");
    assert(strstr(ack_output,"cmd=POSE id=9 ok=0 reason=STALE"));
    reset();tick(4000);
    feed("PERIPH LIVE 1 4000 0 1500 50\n");
    assert(strstr(ack_output,"cmd=LIVE id=1 ok=1 reason=OK"));
    assert(TTLServo_MotionNext(0,1,packet_bytes,&n,now)==1);
    assert(packet_bytes[8]==50&&packet_bytes[9]==0);
    feed("PERIPH STOP 2\nPERIPH LIVE 3 4000 0 1500 0\nPERIPH LIVE 4 4000 0 1500 1024\n");
    assert(state().servo_mode==SERVO_MODE_IDLE);
    assert(strstr(ack_output,"cmd=LIVE id=3 ok=0 reason=SYNTAX"));
    assert(strstr(ack_output,"cmd=LIVE id=4 ok=0 reason=SYNTAX"));
    reset();tick(4000);
    feed("PERIPH WALK 1 4000 3000 50\n");assert(strstr(ack_output,"cmd=WALK id=1 ok=1 reason=OK"));
    assert(state().servo_mask==15&&state().servo_mode==SERVO_MODE_LIVE);
    feed("PERIPH LIVE 2 4000 0 1500\n");assert(strstr(ack_output,"cmd=LIVE id=2 ok=0 reason=BUSY"));
    feed("PERIPH POSE 3 4000 1500 1500 1500 1500\n");assert(strstr(ack_output,"cmd=POSE id=3 ok=0 reason=BUSY"));
    tick(5600);assert(state().servo_mode==SERVO_MODE_IDLE); /* firmware lease, no browser */
    feed("PERIPH WALK 1 5600 3000 50\n");assert(strstr(ack_output,"cmd=WALK id=1 ok=0 reason=OLD_ID"));
    reset();tick(4000);offline=2;
    feed("PERIPH WALK 1 4000 3000 50\n");assert(state().servo_mode==SERVO_MODE_IDLE);
    assert(strstr(ack_output,"cmd=WALK id=1 ok=0 reason=TTL_NOT_READY"));offline=255;
    feed("PERIPH WALK 2 4000 10001 50\nPERIPH WALK 3 4000 3000 49\n");assert(state().servo_mode==SERVO_MODE_IDLE);
    reset();tick(4000);feed("PERIPH WALK 1 4000 3000 50\n");settle_motion();tick(4001);
    tick(4021);assert(TTLServo_IsStreaming());
    {uint32_t t,id=2;char line[64];for(t=4041;t<=7001;t+=20U){
        if((t-4041U)%400U==0U){sprintf(line,"PERIPH KEEP %lu %lu\n",(unsigned long)id++,(unsigned long)now);feed(line);}
        tick(t);settle_motion();
        assert(state().servo_us[0]==0U||(state().servo_us[0]>=500U&&state().servo_us[0]<=2500U));
    }}
    assert(state().servo_mode==SERVO_MODE_IDLE&&!TTLServo_IsStreaming());
    Peripherals_PrintStatus();assert(strstr(ack_output,"walk_state=DONE"));
    {unsigned kind;char line[80];static const char *names[]={"FORWARD","BACKWARD","LEFT","RIGHT","PET"};
    for(kind=0;kind<5;++kind){
        reset();tick(4000);sprintf(line,"PERIPH WALK 1 4000 %u %u %u\n",kind==4?6000:3000,kind==4?100:50,kind);
        feed(line);assert(state().servo_mode==SERVO_MODE_LIVE);Peripherals_PrintStatus();assert(strstr(ack_output,names[kind]));
    }}
    reset();tick(4000);feed("PERIPH WALK 1 4000 3000 50 5\nPERIPH WALK 2 4000 3000 50 4\n");
    assert(state().servo_mode==SERVO_MODE_IDLE&&strstr(ack_output,"reason=SYNTAX"));
    /* An already-held sensor at boot must never start the local response. */
    reset();raw_touch=1;tick(300);tick(320);tick(2000);assert(state().servo_mode==SERVO_MODE_IDLE);
    raw_touch=0;tick(2020);tick(2040);tick(2341);raw_touch=1;tick(2360);tick(2380);
    assert(state().servo_mode==SERVO_MODE_LIVE);Peripherals_PrintStatus();
    assert(strstr(ack_output,"walk_kind=PET walk_local=1 pet_proto=1 pet_events=1"));
    settle_motion();tick(2381);
    {uint32_t t;for(t=2401;t<=8381;t+=20U){tick(t);settle_motion();}}
    assert(state().servo_mode==SERVO_MODE_IDLE); /* finite local action needs no browser KEEP */
    tick(11000);assert(state().servo_mode==SERVO_MODE_IDLE); /* held touch never repeats */
    Peripherals_PrintStatus();assert(strstr(ack_output,"walk_state=DONE"));
    reset();tick(2000);tick(2301);feed("PERIPH TEACH 1 2301\n");raw_touch=1;tick(2320);tick(2340);
    assert(TTLServo_IsTeaching());Peripherals_PrintStatus();assert(strstr(ack_output,"pet_events=0 pet_reason=BUSY"));
    feed("PERIPH STOP 2\n");settle_motion();tick(3000);assert(state().servo_mode==SERVO_MODE_IDLE);
    reset();tick(2000);tick(2301);battery.faults|=BATTERY_FAULT_BUS;raw_touch=1;tick(2320);tick(2340);
    assert(state().servo_mode==SERVO_MODE_IDLE);Peripherals_PrintStatus();assert(strstr(ack_output,"pet_reason=BATTERY"));
    feedback_baud=117647;reset();tick(4000);feed("PERIPH TEACH 1 4000\n");assert(TTLServo_IsTeaching());
    feed("PERIPH STOP 2\n");settle_motion();feed("PERIPH WALK 3 4000 3000 50 1\n");
    assert(strstr(ack_output,"cmd=WALK id=3 ok=1"));settle_motion();tick(4001);tick(4021);assert(TTLServo_IsStreaming());
    feedback_flags=1;tick(4041);assert(state().servo_mode==SERVO_MODE_IDLE);feedback_flags=0;
    puts("Teaching and motion suite passed: compatible baud, five profiles, finite head response, busy/battery/fault interlocks, STOP and lease.");return 0;
}
