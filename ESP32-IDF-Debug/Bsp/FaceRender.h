#pragma once
#include <stdint.h>
#include "FaceLogic.h"
/* Logical landscape bounds; native/270-degree buffer is H pixels wide. */
#define FACE_X 102U
#define FACE_Y 94U
#define FACE_W 298U
#define FACE_H 174U
#define FACE_NATIVE_Y (502U - FACE_X - FACE_W)
#define FACE_BYTES (FACE_W * FACE_H * 2U)
void FaceRender_Draw(uint8_t *native_rgb565, FaceMood mood, uint32_t now_ms);
