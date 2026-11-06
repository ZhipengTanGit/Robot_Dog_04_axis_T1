/* Share the UART, battery and GPIO host harness; execute only TTL cases. */
#define main peripheral_legacy_tests
#include "test_peripherals.c"
#undef main
#include "TTLServo_Motion.h"
static uint8_t offline=255;
void TTLServo_Init(void){TTLServo_MotionInit();}
void TTLServo_Task(uint32_t tick_value){(void)tick_value;}
void TTLServo_PrintStatus(uint32_t tick_value){(void)tick_value;}
void TTLServo_GetStatus(uint8_t ch,uint32_t tick_value,TTLServo_Status *s)
{(void)tick_value;memset(s,0,sizeof(*s));s->valid=(uint8_t)(ch!=offline);s->baud=115200UL;}
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
    puts("Teaching commands passed: four-leg atomic validation, lock, STOP fence, limits and stale command rejection.");return 0;
}
