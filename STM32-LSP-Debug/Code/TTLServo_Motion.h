#ifndef TTL_SERVO_MOTION_H
#define TTL_SERVO_MOTION_H
#include "TTLServo.h"
void TTLServo_MotionInit(void);
int TTLServo_SetTarget(uint8_t channel,uint16_t pulse,uint32_t now);
int TTLServo_SetTargetSpeed(uint8_t channel,uint16_t pulse,uint16_t speed,uint32_t now);
int TTLServo_SetPose(const uint16_t pulses[4],uint32_t now);
void TTLServo_StopAll(void);
void TTLServo_BeginTeach(void);
void TTLServo_EndTeach(void);
uint8_t TTLServo_IsTeaching(void);
uint8_t TTLServo_ActiveMask(void);
uint8_t TTLServo_TorqueState(uint8_t channel);
int TTLServo_MotionNext(uint8_t channel,uint8_t id,uint8_t *packet,size_t *size,uint32_t now);
void TTLServo_MotionComplete(uint8_t channel,int op,const uint8_t *reply,size_t size,int result,uint32_t now);
#endif
