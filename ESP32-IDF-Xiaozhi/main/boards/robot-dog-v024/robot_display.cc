#include "robot_display.h"
#include "Bsp_Display.h"
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

RobotDisplay::RobotDisplay() {
    width_ = 502;
    height_ = 410;
    auto result = Display_Start();
    if (result != ESP_OK) ESP_LOGE("RobotDisplay", "Start failed: %s", esp_err_to_name(result));
}
void RobotDisplay::SetStatus(const char* status) {
    Display::SetStatus(status);
}
bool RobotDisplay::Ready() {
    Display_Status s;
    Display_GetStatus(&s);
    return s.configured;
}
bool RobotDisplay::ShowExpression(const std::string& name, bool manual) {
    unsigned mood;
    if (name == "neutral" || name == "calm") mood = 0;
    else if (name == "curious" || name == "thinking" || name == "confused" ||
             name == "surprised" || name == "shocked" || name == "angry" || name == "question") mood = 1;
    else if (name == "happy" || name == "laughing" || name == "funny" || name == "loving" ||
             name == "confident" || name == "cool" || name == "excited" || name == "winking" || name == "kissy") mood = 2;
    else if (name == "sleepy" || name == "sad" || name == "crying" || name == "embarrassed") mood = 3;
    else if (name == "comfort" || name == "relaxed" || name == "delicious") mood = 4;
    else return false;
    const auto now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (manual) manual_until_.store(now + 20000);
    else {
        const auto until = manual_until_.load();
        if (until && static_cast<int32_t>(until - now) > 0) return true;
    }
    Display_SetVoiceEmotion(mood);
    return true;
}
void RobotDisplay::SetEmotion(const char* emotion) {
    Display::SetEmotion(emotion);
    if (emotion) (void)ShowExpression(emotion);
}
std::string RobotDisplay::StatusJson() {
    Display_Status s;
    Display_GetStatus(&s);
    auto root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "configured", s.configured);
    cJSON_AddStringToObject(root, "state", s.state);
    cJSON_AddStringToObject(root, "init_stage", s.init_stage);
    cJSON_AddStringToObject(root, "error", esp_err_to_name(s.error));
    cJSON_AddStringToObject(root, "expression", s.expression);
    cJSON_AddStringToObject(root, "page", s.ui_mode);
    cJSON_AddNumberToObject(root, "face_frames", s.face_frames);
    cJSON_AddNumberToObject(root, "write_errors", s.write_errors);
    cJSON_AddNumberToObject(root, "te_overruns", s.te_overruns);
    cJSON_AddBoolToObject(root, "touch_present", s.touch_present);
    cJSON_AddNumberToObject(root, "screen_taps", s.screen_taps);
    cJSON_AddNumberToObject(root, "head_touch_events", s.head_touch_events);
    cJSON_AddBoolToObject(root, "manual_expression_hold", static_cast<int32_t>(manual_until_.load() - static_cast<uint32_t>(esp_timer_get_time() / 1000)) > 0);
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return result;
}
