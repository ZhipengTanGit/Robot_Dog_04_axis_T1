#include "Bsp_I2cDiagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    I2cDiagnostics_Status scan;
    memset(&scan, 0, sizeof(scan));
    bool start;
    assert(I2cDiagnostics_ResultRequest(&scan, "1111111111111111", 100, &start) == ESP_OK && start);
    assert(scan.running && !scan.completed && scan.started_ms == 100 && scan.sequence == 1);
    assert(scan.sda == 8 && scan.scl == 19);
    assert(I2cDiagnostics_ResultRequest(&scan, "1111111111111111", 101, &start) == ESP_OK && !start);
    assert(I2cDiagnostics_ResultRequest(&scan, "2222222222222222", 102, &start) == ESP_ERR_INVALID_STATE && !start);
    assert(scan.sequence == 1 && scan.started_ms == 100);
    I2cDiagnostics_ResultRecord(&scan, 0x00, ESP_OK);
    I2cDiagnostics_ResultRecord(&scan, 0x78, ESP_OK);
    assert(scan.scanned == 0);
    for (unsigned address = 0x08; address <= 0x77; ++address)
        I2cDiagnostics_ResultRecord(&scan, address, address == 0x20 ? ESP_OK : ESP_ERR_NOT_FOUND);
    I2cDiagnostics_ResultFinish(&scan, 1500, false);
    assert(!scan.running && scan.completed && !strcmp(scan.state, "COMPLETE"));
    assert(scan.scanned == 112 && scan.nack_count == 111 && scan.count == 1 && scan.addresses[0] == 0x20);
    assert(scan.error_count == 0 && scan.timeout_count == 0 && scan.error == ESP_OK && !scan.audio_restart_required);
    assert(scan.completed_ms == 1500 && !scan.codec_ack && !scan.codec_alternate_ack);
    assert(I2cDiagnostics_ResultRequest(&scan, "1111111111111111", 1501, &start) == ESP_OK && !start);
    assert(scan.completed && scan.sequence == 1);

    assert(I2cDiagnostics_ResultRequest(&scan, "2222222222222222", 2000, &start) == ESP_OK && start);
    for (unsigned address = 0x08; address <= 0x77; ++address) {
        esp_err_t result = ESP_ERR_NOT_FOUND;
        if (address == 0x10 || address == 0x11 || address == 0x20) result = ESP_OK;
        if (address == 0x40) result = ESP_ERR_TIMEOUT;
        if (address == 0x41) result = ESP_ERR_INVALID_STATE;
        I2cDiagnostics_ResultRecord(&scan, address, result);
    }
    I2cDiagnostics_ResultFinish(&scan, 3500, false);
    assert(scan.completed && !strcmp(scan.state, "SCAN_ERRORS") && scan.timeout_count == 1 && scan.error_count == 1);
    assert(scan.error == ESP_ERR_TIMEOUT && scan.codec_ack && scan.codec_alternate_ack && scan.audio_restart_required);
    assert(scan.count == 3 && scan.addresses[0] == 0x10 && scan.addresses[2] == 0x20);
    I2cDiagnostics_ResultFinish(&scan, 3600, true);
    assert(!scan.audio_restart_required);

    assert(I2cDiagnostics_ResultRequest(&scan, "3333333333333333", 4000, &start) == ESP_OK && start);
    I2cDiagnostics_ResultRecord(&scan, 0x08, ESP_ERR_TIMEOUT);
    I2cDiagnostics_ResultFinish(&scan, 9000, false);
    assert(!scan.completed && !strcmp(scan.state, "ABORTED") && scan.timeout_count == 1 && scan.scanned == 1);
    I2cDiagnostics_ResultRecord(&scan, 0x09, ESP_OK);
    assert(scan.scanned == 1);
    I2cDiagnostics_Status before = scan;
    const char *invalid[] = {NULL, "", "123", "111111111111111g", "11111111111111111", "111111111111111\n"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(I2cDiagnostics_ResultRequest(&scan, invalid[i], 9010, &start) == ESP_ERR_INVALID_ARG && !start);
        assert(!memcmp(&scan, &before, sizeof(scan)));
    }
    assert(I2cDiagnostics_ResultRequest(&scan, "4444444444444444", 10000, &start) == ESP_OK && start);
    for (unsigned address = 0x08; address <= 0x77; ++address) {
        if (address == 0x10) I2cDiagnostics_ResultRecordCodec(&scan, address, ESP_OK, true, 0x06);
        else if (address == 0x11) I2cDiagnostics_ResultRecordCodec(&scan, address, ESP_ERR_INVALID_STATE, true, 0xff);
        else I2cDiagnostics_ResultRecord(&scan, address, address == 0x20 ? ESP_OK : ESP_ERR_NOT_FOUND);
    }
    I2cDiagnostics_ResultFinish(&scan, 11500, true);
    assert(scan.completed && !strcmp(scan.state, "COMPLETE") && scan.scanned == 112);
    assert(scan.count == 2 && scan.addresses[0] == 0x10 && scan.addresses[1] == 0x20);
    assert(scan.codec_read_attempted && scan.codec_read_error == ESP_OK && scan.codec_reg00 == 0x06);
    assert(scan.codec_alternate_read_attempted && scan.codec_alternate_read_error == ESP_ERR_INVALID_STATE);
    assert(!scan.codec_alternate_ack && scan.codec_unconfirmed_count == 1 && scan.codec_alternate_reg00 == 0);
    assert(scan.error == ESP_OK && scan.error_count == 0 && scan.nack_count == 109 && !scan.audio_restart_required);
    assert(I2cDiagnostics_ResultRequest(&scan, "5555555555555555", 12000, &start) == ESP_OK && start);
    assert(!scan.codec_read_attempted && !scan.codec_alternate_read_attempted && scan.codec_unconfirmed_count == 0);
    I2cDiagnostics_ResultRecordCodec(&scan, 0x10, ESP_ERR_TIMEOUT, true, 0x06);
    assert(scan.timeout_count == 1 && scan.error == ESP_ERR_TIMEOUT && !scan.codec_ack);
    I2cDiagnostics_ResultRecordCodec(&scan, 0x11, ESP_ERR_INVALID_STATE, false, 0x06);
    assert(scan.error_count == 1 && scan.codec_unconfirmed_count == 0 && !scan.codec_alternate_read_attempted);
    puts("i2c_diagnostics: 6 test groups passed (ACK/NACK, timeout/error, interrupted scan, ID/idempotency/busy, 50k codec read, codec timeout/setup failures)");
    return 0;
}
