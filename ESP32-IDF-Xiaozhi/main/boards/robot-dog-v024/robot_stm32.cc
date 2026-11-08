#include "robot_stm32.h"
#include "Bsp_Display.h"
#include "Bsp_BoardBus.h"
#include <driver/uart.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <algorithm>
#include <climits>
#include <cstdio>

namespace {
uint32_t Now() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
std::string Json(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text ? text : "{\"ok\":false,\"reason\":\"NO_MEMORY\"}";
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}
std::string Result(bool ok, const char* reason, uint32_t id = 0) {
    auto root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "reason", reason);
    if (id) cJSON_AddNumberToObject(root, "command_id", id);
    return Json(root);
}
}

esp_err_t RobotStm32::Start() {
    if (started_) return ESP_OK;
    uart_config_t config = {};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    auto error = uart_param_config(UART_NUM_1, &config);
    if (error == ESP_OK) error = uart_set_pin(UART_NUM_1, 4, 5, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (error == ESP_OK) error = uart_driver_install(UART_NUM_1, 8192, 2048, 0, nullptr, 0);
    if (error != ESP_OK) return error;
    if (xTaskCreate([](void* p) { static_cast<RobotStm32*>(p)->Task(); }, "stm32_link", 6144, this, 3, nullptr) != pdPASS) {
        uart_driver_delete(UART_NUM_1);
        return ESP_ERR_NO_MEM;
    }
    started_ = true;
    return ESP_OK;
}
bool RobotStm32::Write(const std::string& line) {
    std::lock_guard lock(tx_mutex_);
    return uart_write_bytes(UART_NUM_1, line.data(), line.size()) == static_cast<int>(line.size());
}
uint32_t RobotStm32::NextIdLocked() {
    next_id_ = std::max(next_id_, telemetry_.last_id);
    return next_id_ == UINT32_MAX ? 0 : ++next_id_;
}
void RobotStm32::Line(const std::string& line) {
    const uint32_t now = Now();
    if (line.compare(0, 7, "PERIPH ") == 0) {
        robot::Telemetry parsed;
        if (!parsed.Parse(line, now)) return;
        {
            std::lock_guard lock(state_mutex_);
            if (telemetry_.present && static_cast<int32_t>(parsed.uptime_ms - telemetry_.uptime_ms) < 0) {
                leased_ = false;
                next_id_ = 0;
                if (pending_id_) { ack_seen_ = true; ack_ok_ = false; ack_reason_ = "STM32_RESET"; }
            }
            telemetry_ = parsed;
            if (leased_ && walking_ && parsed.walk_id == action_id_ &&
                (parsed.walk_state == "DONE" || parsed.walk_state == "STOPPED" || parsed.walk_state == "FAILED")) {
                leased_ = false;
                ESP_LOGI("RobotSTM32", "Action ended: state=%s reason=%s", parsed.walk_state.c_str(), parsed.walk_reason.c_str());
            }
        }
        Display_UpdateRobot(nullptr, line.c_str());
    } else if (line.compare(0, 4, "BAT ") == 0) {
        Display_UpdateRobot(line.c_str(), nullptr);
    } else if (line.compare(0, 11, "PERIPH_ACK ") == 0) {
        uint32_t id, ok;
        std::string_view reason;
        if (!robot::UnsignedToken(line, "id", id) || !robot::UnsignedToken(line, "ok", ok) || ok > 1 ||
            !robot::Token(line, "reason", reason)) return;
        std::lock_guard lock(state_mutex_);
        if (id == pending_id_) {
            ack_seen_ = true; ack_ok_ = ok == 1; ack_reason_ = reason;
            ESP_LOGI("RobotSTM32", "Command %lu: ok=%lu reason=%s", static_cast<unsigned long>(id), static_cast<unsigned long>(ok), ack_reason_.c_str());
        }
    }
}
void RobotStm32::MaintainLease(uint32_t now) {
    std::unique_lock command_lock(command_mutex_, std::try_to_lock);
    if (!command_lock.owns_lock()) return;
    std::string message;
    {
        std::lock_guard lock(state_mutex_);
        if (!leased_) return;
        bool expire = !telemetry_.Fresh(now) || !telemetry_.servo_allowed ||
            static_cast<int32_t>(now - lease_deadline_) >= 0;
        if (expire) {
            leased_ = false;
            auto id = NextIdLocked();
            message = "PERIPH STOP " + std::to_string(id) + "\r\n";
        } else {
            auto id = NextIdLocked();
            if (!id) { leased_ = false; message = "PERIPH STOP\r\n"; }
            else message = "PERIPH KEEP " + std::to_string(id) + " " + std::to_string(telemetry_.EstimatedUptime(now)) + "\r\n";
        }
    }
    if (!Write(message)) {
        std::lock_guard lock(state_mutex_);
        leased_ = false; // STM32's 1500 ms hardware lease will also expire.
    }
}
void RobotStm32::Task() {
    std::string line;
    line.reserve(4096);
    bool overflow = false;
    uint32_t last_poll = Now() - 1000, last_keep = Now();
    for (;;) {
        uint8_t bytes[512];
        int count = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), pdMS_TO_TICKS(20));
        for (int i = 0; i < count; ++i) {
            uint8_t c = bytes[i];
            if (c == '\r' || c == '\n') {
                if (!line.empty() && !overflow) Line(line);
                line.clear(); overflow = false;
            } else if (c >= 32 && c < 127 && line.size() < 8191) line.push_back(static_cast<char>(c));
            else overflow = true;
        }
        uint32_t now = Now();
        if (now - last_poll >= 1000) { last_poll = now; Write("PERIPH STATUS\r\n"); BoardBus_RefreshStatus(); }
        if (now - last_keep >= 400) { last_keep = now; MaintainLease(now); }
    }
}
std::string RobotStm32::EnvironmentJson() {
    robot::Telemetry t;
    { std::lock_guard lock(state_mutex_); t = telemetry_; }
    uint32_t now = Now();
    auto root = cJSON_CreateObject();
    bool valid = t.SensorFresh(now);
    cJSON_AddBoolToObject(root, "ok", valid);
    cJSON_AddStringToObject(root, "source", "onboard_AHT20");
    cJSON_AddStringToObject(root, "measurement", "local ambient temperature and relative humidity, not online weather");
    if (valid) {
        cJSON_AddNumberToObject(root, "temperature_celsius", t.temperature_cc / 100.0);
        cJSON_AddNumberToObject(root, "relative_humidity_percent", t.humidity_cpct / 100.0);
        cJSON_AddNumberToObject(root, "sample_age_ms", uint64_t(t.sensor_age_ms) + uint32_t(now - t.received_ms));
    } else {
        cJSON_AddStringToObject(root, "reason", !t.Fresh(now) ? "STM32_OFFLINE" : "SENSOR_INVALID_OR_STALE");
        cJSON_AddStringToObject(root, "sensor_state", t.sensor_state.c_str());
        cJSON_AddStringToObject(root, "instruction", "Report measurement unavailable; do not substitute weather or invent a value.");
    }
    return Json(root);
}
std::string RobotStm32::StatusJson() {
    robot::Telemetry t;
    bool lease;
    { std::lock_guard lock(state_mutex_); t = telemetry_; lease = leased_; }
    auto root = cJSON_CreateObject();
    bool fresh = t.Fresh(Now());
    cJSON_AddBoolToObject(root, "connected", fresh);
    cJSON_AddBoolToObject(root, "servo_allowed", fresh && t.servo_allowed);
    cJSON_AddBoolToObject(root, "teaching", t.teaching);
    cJSON_AddBoolToObject(root, "voice_action_lease", lease);
    cJSON_AddNumberToObject(root, "active_servo_mask", fresh ? t.servo_mask : 0);
    cJSON_AddStringToObject(root, "walk_state", fresh ? t.walk_state.c_str() : "UNKNOWN");
    cJSON_AddStringToObject(root, "walk_reason", fresh ? t.walk_reason.c_str() : "STM32_OFFLINE");
    cJSON_AddNumberToObject(root, "walk_id", t.walk_id);
    return Json(root);
}
std::string RobotStm32::Action(const std::string& name, unsigned duration) {
    const bool stop = name == "stop", center = name == "center", pet = name == "pet";
    unsigned kind;
    if (name == "forward") kind = 0;
    else if (name == "backward") kind = 1;
    else if (name == "left") kind = 2;
    else if (name == "right") kind = 3;
    else if (pet) { kind = 4; duration = 6000; }
    else if (stop || center) kind = 0;
    else return Result(false, "UNKNOWN_ACTION");
    if (duration < 1000 || duration > 10000) return Result(false, "DURATION_RANGE");
    std::lock_guard command_lock(command_mutex_);
    uint32_t id, stamp;
    {
        std::lock_guard lock(state_mutex_);
        uint32_t now = Now();
        if (!started_) return Result(false, "UART_UNAVAILABLE");
        if (!stop && !telemetry_.Fresh(now)) return Result(false, "STM32_OFFLINE");
        if (!stop && !telemetry_.servo_allowed) return Result(false, "BATTERY_INTERLOCK");
        if (!stop && telemetry_.teaching) return Result(false, "TEACH_LOCK");
        if (!stop && (leased_ || telemetry_.walk_state == "PREPARE" || telemetry_.walk_state == "RUN")) return Result(false, "BUSY");
        id = NextIdLocked();
        if (!id) return Result(false, "COMMAND_ID_EXHAUSTED");
        stamp = telemetry_.EstimatedUptime(now);
        pending_id_ = id; ack_seen_ = ack_ok_ = false; ack_reason_.clear();
        if (stop) leased_ = false;
    }
    char command[120];
    if (stop) snprintf(command, sizeof(command), "PERIPH STOP %lu\r\n", static_cast<unsigned long>(id));
    else if (center) snprintf(command, sizeof(command), "PERIPH POSE %lu %lu 1500 1500 1500 1500\r\n", static_cast<unsigned long>(id), static_cast<unsigned long>(stamp));
    else snprintf(command, sizeof(command), "PERIPH WALK %lu %lu %u 100 %u\r\n", static_cast<unsigned long>(id), static_cast<unsigned long>(stamp), duration, kind);
    bool written = Write(command);
    uint32_t began = Now();
    while (written && uint32_t(Now() - began) < 1200) {
        {
            std::lock_guard lock(state_mutex_);
            if (ack_seen_) {
                bool ok = ack_ok_;
                std::string reason = ack_reason_;
                pending_id_ = 0;
                if (ok && !stop) {
                    leased_ = true; walking_ = !center; action_id_ = id;
                    lease_started_ = Now();
                    lease_deadline_ = lease_started_ + duration + (center ? 0 : 5500);
                }
                return Result(ok, ok ? (stop ? "STOP_ACCEPTED" : "ACCEPTED_NOT_COMPLETED") : reason.c_str(), id);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    { std::lock_guard lock(state_mutex_); pending_id_ = 0; leased_ = false; }
    // An unconfirmed write may still have started; send an unconditional stop.
    Write("\r\nPERIPH STOP\r\n");
    return Result(false, written ? "ACK_TIMEOUT_STOP_SENT" : "UART_WRITE_FAILED", id);
}
