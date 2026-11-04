#include "CameraSerialLogic.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
int main(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const char *id = "0123456789abcdef";
    char output[CAMERA_SERIAL_LINE_BYTES];
    uint8_t payload[CAMERA_SERIAL_CHUNK_BYTES];
    for (size_t length = 1; length <= sizeof(payload); ++length) {
        for (size_t i = 0; i < length; ++i) payload[i] = (uint8_t)(i * 73 + length);
        size_t count = CameraSerial_FormatData(output, sizeof(output), id, 511, payload, length);
        assert(count && count < sizeof(output));
        assert(fwrite(output, 1, count, stdout) == count);
    }
    const size_t sizes[] = {0, 4, 1024, 1025, CAMERA_SERIAL_MAX_BYTES};
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        size_t length = sizes[k]; uint8_t *data = malloc(length ? length : 1);
        assert(data);
        for (size_t i = 0; i < length; ++i) data[i] = (uint8_t)(i * 53 + length);
        printf("TEST_CRC %lu %lu\n", (unsigned long)length, (unsigned long)CameraSerial_Crc32(data, length));
        free(data);
    }
    assert(CameraSerial_Crc32((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    CameraSerial_Metadata meta = {.bytes = CAMERA_SERIAL_MAX_BYTES, .width = 1920, .height = 1080,
        .sequence = 123, .timestamp_ms = UINT32_MAX, .crc32 = UINT32_MAX};
    size_t count = CameraSerial_FormatBegin(output, sizeof(output), id, &meta);
    assert(count); assert(fwrite(output, 1, count, stdout) == count);
    count = CameraSerial_FormatEnd(output, sizeof(output), id, &meta);
    assert(count); assert(fwrite(output, 1, count, stdout) == count);
    count = CameraSerial_FormatError(output, sizeof(output), id, 259, "BUSY");
    assert(count); assert(fwrite(output, 1, count, stdout) == count);
    assert(!CameraSerial_FormatData(output, sizeof(output), id, 512, payload, 1));
    assert(!CameraSerial_FormatData(output, sizeof(output), id, 0, payload, 0));
    assert(!CameraSerial_FormatData(output, 20, id, 0, payload, 1));
    assert(!CameraSerial_FormatData(output, sizeof(output), "0123456789abcdeF", 0, payload, 1));
    assert(!CameraSerial_FormatError(output, sizeof(output), id, 1, "bad\"detail"));
    ++meta.bytes; assert(!CameraSerial_FormatBegin(output, sizeof(output), id, &meta));
    return 0;
}
