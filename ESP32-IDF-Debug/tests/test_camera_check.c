#include "CameraCheckLogic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_request(void)
{
    Camera_CheckStatus check;
    memset(&check, 0, sizeof(check));
    bool start;
    assert(CameraCheck_Begin(&check, "0123456789abcdef", 100, &start) == ESP_OK && start);
    assert(check.running && !check.completed && check.sequence == 1 && check.started_ms == 100);
    assert(check.frame_error == ESP_ERR_INVALID_STATE && !check.usable);
    assert(CameraCheck_Begin(&check, "0123456789abcdef", 101, &start) == ESP_OK && !start);
    assert(CameraCheck_Begin(&check, "1111111111111111", 102, &start) == ESP_ERR_INVALID_STATE && !start);
    CameraCheck_Finish(&check, "SCCB_PROBE", ESP_ERR_NOT_FOUND, 200);
    assert(check.completed && !check.running && !check.usable && !strcmp(check.state, "FAILED"));
    assert(CameraCheck_Begin(&check, "0123456789abcdef", 201, &start) == ESP_OK && !start);
    assert(CameraCheck_Begin(&check, "1111111111111111", 202, &start) == ESP_OK && start);
    assert(check.sequence == 2 && !check.completed && check.frame_error == ESP_ERR_INVALID_STATE);
    Camera_CheckStatus before = check;
    const char *invalid[] = {NULL, "", "0123", "0123456789abcdeF", "0123456789abcdeg", "0123456789abcdef0"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(CameraCheck_Begin(&check, invalid[i], 300, &start) == ESP_ERR_INVALID_ARG && !start);
        assert(!memcmp(&check, &before, sizeof(check)));
    }
}
static void test_success_requires_all_phases(void)
{
    Camera_CheckStatus check;
    memset(&check, 0, sizeof(check));
    check.expander_ok = check.sccb_ack = check.id_ok = check.init_ok = true;
    CameraCheck_Finish(&check, "FRAME", ESP_OK, 100);
    assert(!check.usable && check.error == ESP_ERR_INVALID_STATE);
    check.frame_ok = true;
    CameraCheck_Finish(&check, "DONE", ESP_OK, 101);
    assert(check.usable && check.completed && !check.running && !strcmp(check.state, "PASSED"));
    check.id_ok = false;
    CameraCheck_Finish(&check, "SENSOR_ID", ESP_OK, 102);
    assert(!check.usable);
    check.id_ok = true;
    CameraCheck_Finish(&check, "CLEANUP", ESP_ERR_TIMEOUT, 103);
    assert(!check.usable && check.error == ESP_ERR_TIMEOUT);
}
static void test_jpeg_structure(void)
{
    const uint8_t jpeg[] = {
        0xff,0xd8,
        0xff,0xe0,0,4,0,0,
        0xff,0xc0,0,11,8,1,0xe0,2,0x80,1,1,0x11,0,
        0xff,0xda,0,8,1,1,0,0,0x3f,0,
        0x11,0x22,0x33,0xff,0xd9
    };
    assert(CameraCheck_JpegValid(jpeg, sizeof(jpeg), 640, 480));
    assert(!CameraCheck_JpegValid(jpeg, sizeof(jpeg), 320, 240));
    assert(!CameraCheck_JpegValid(jpeg, sizeof(jpeg), 0, 480));
    assert(!CameraCheck_JpegValid(NULL, sizeof(jpeg), 640, 480));
    for (size_t length = 0; length < sizeof(jpeg); ++length)
        assert(!CameraCheck_JpegValid(jpeg, length, 640, 480));
    /* A large, structurally complete JPEG must fit the diagnostic budget. */
    static uint8_t budget[CAMERA_CHECK_MAX_JPEG_BYTES + 1U];
    memset(budget, 0x11, sizeof(budget));
    memcpy(budget, jpeg, sizeof(jpeg) - 5U);
    budget[CAMERA_CHECK_MAX_JPEG_BYTES - 2U] = 0xff;
    budget[CAMERA_CHECK_MAX_JPEG_BYTES - 1U] = 0xd9;
    assert(CameraCheck_JpegValid(budget, CAMERA_CHECK_MAX_JPEG_BYTES, 640, 480));
    budget[CAMERA_CHECK_MAX_JPEG_BYTES - 2U] = 0x11;
    budget[CAMERA_CHECK_MAX_JPEG_BYTES - 1U] = 0xff;
    budget[CAMERA_CHECK_MAX_JPEG_BYTES] = 0xd9;
    assert(!CameraCheck_JpegValid(budget, sizeof(budget), 640, 480));
    uint8_t broken[sizeof(jpeg)];
    memcpy(broken, jpeg, sizeof(jpeg));
    broken[4] = 0xff; broken[5] = 0xff;
    assert(!CameraCheck_JpegValid(broken, sizeof(broken), 640, 480));
    memcpy(broken, jpeg, sizeof(jpeg)); broken[11] = 0;
    assert(!CameraCheck_JpegValid(broken, sizeof(broken), 640, 480));
    memcpy(broken, jpeg, sizeof(jpeg)); broken[9] = 0xe1;
    assert(!CameraCheck_JpegValid(broken, sizeof(broken), 640, 480)); /* SOS without SOF */
    memcpy(broken, jpeg, sizeof(jpeg)); broken[24] = 7;
    assert(!CameraCheck_JpegValid(broken, sizeof(broken), 640, 480)); /* Bad SOS length */
    memcpy(broken, jpeg, sizeof(jpeg)); broken[17] = 4;
    assert(!CameraCheck_JpegValid(broken, sizeof(broken), 640, 480)); /* Bad SOF components */
    const uint8_t empty_jpeg[] = {0xff,0xd8,0xff,0xd9};
    assert(!CameraCheck_JpegValid(empty_jpeg, sizeof(empty_jpeg), 640, 480));
}
int main(void)
{
    test_request(); test_success_requires_all_phases(); test_jpeg_structure();
    puts("camera_check: 3 groups passed (ID/retry/busy, phased success, JPEG structure/bounds)");
    return 0;
}
