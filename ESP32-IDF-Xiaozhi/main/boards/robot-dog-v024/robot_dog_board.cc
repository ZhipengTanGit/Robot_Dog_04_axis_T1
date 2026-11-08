#include "wifi_board.h"
#include "robot_audio_codec.h"
#include "robot_display.h"
#include "robot_stm32.h"
#include "Bsp_BoardBus.h"
#include "mcp_server.h"
#include "application.h"
#include "ssid_manager.h"
#include "settings.h"
#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// Optional local provisioning seed; never checked into source control.
#if __has_include("wifi_seed.local.h")
#include "wifi_seed.local.h"
#endif

namespace {
constexpr char TAG[] = "RobotDog";
}

class RobotDogBoard : public WifiBoard {
    i2c_master_bus_handle_t bus_ = nullptr;
    RobotDogAudioCodec* codec_ = nullptr;
    RobotDisplay* display_ = nullptr;
    RobotStm32 stm32_;

    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.environment.get_temperature_humidity",
            "Read the robot's onboard AHT20 LOCAL room temperature and humidity. "
            "Use for 当前环境温湿度、室内温度、周围湿度. This is a physical sensor, not city weather. "
            "If ok=false report the sensor unavailable, never substitute online weather or guessed values.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                auto json = stm32_.EnvironmentJson();
                ESP_LOGI(TAG, "MCP local_environment=%s", json.c_str());
                return json;
            });
        mcp.AddTool("self.robot.get_motion_status",
            "Read actual STM32 motion state, battery interlock and completion/failure of the previous robot action.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue { return stm32_.StatusJson(); });
        mcp.AddTool("self.robot.perform_action",
            "Control the robot body on explicit user request. action: forward=前进, backward=后退, "
            "left=左转, right=右转, pet=摸头撒娇动作, center=四腿回中, stop=停止并释放扭矩. "
            "duration_seconds: 1..10, default 3; pet uses its fixed 6-second profile. "
            "Call stop before changing an active motion. ACCEPTED_NOT_COMPLETED means accepted, not finished; "
            "get_motion_status reports actual result. Report any refusal honestly. 握手 is not supported by this firmware.",
            PropertyList({Property("action", kPropertyTypeString),
                          Property("duration_seconds", kPropertyTypeInteger, 3, 1, 10)}),
            [this](const PropertyList& p) -> ReturnValue {
                return stm32_.Action(p["action"].value<std::string>(), p["duration_seconds"].value<int>() * 1000);
            });
        mcp.AddTool("self.screen.set_expression",
            "Show an animated face on the robot screen. expression: neutral, curious, happy, sleepy, comfort. "
            "Use for requests to change facial expression. Body motion is a separate tool.",
            PropertyList({Property("expression", kPropertyTypeString)}),
            [this](const PropertyList& p) -> ReturnValue {
                if (!display_->Ready()) throw std::runtime_error("Screen is unavailable; do not claim expression displayed");
                auto expression = p["expression"].value<std::string>();
                if (!display_->ShowExpression(expression, true)) throw std::runtime_error("Unknown expression");
                ESP_LOGI(TAG, "MCP expression=%s", expression.c_str());
                return std::string("{\"ok\":true,\"state\":\"QUEUED\",\"screen\":") + display_->StatusJson() + "}";
            });
    }

    void ConsoleTask() {
        char line[80] = {};
        size_t used = 0;
        bool overflow = false;
        ESP_LOGI(TAG, "Console: chat | status | environment | robot-status | display-status | expression NAME | action NAME | volume 0..100 | wifi-config");
        while (true) {
            uint8_t c;
            if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) != 1) {
                continue;
            }
            if (c == '\r' || c == '\n') {
                if (used != 0 && !overflow) {
                    line[used] = 0;
                    HandleCommand(line);
                }
                used = 0;
                overflow = false;
            } else if (used < sizeof(line) - 1) {
                line[used++] = static_cast<char>(c);
            } else {
                overflow = true;
            }
        }
    }

    void HandleCommand(const char* command) {
        auto& app = Application::GetInstance();
        if (strcmp(command, "chat") == 0) {
            app.ToggleChatState();
        } else if (strcmp(command, "status") == 0) {
            ESP_LOGI(TAG, "state=%d codec_ready=%d codec_error=%s volume=%d free_heap=%lu",
                     static_cast<int>(app.GetDeviceState()), codec_->ready(),
                     esp_err_to_name(codec_->last_error()),
                     codec_->output_volume(),
                     static_cast<unsigned long>(esp_get_free_heap_size()));
            ESP_LOGI(TAG, "audio source_peak=%lu pcm_peak=%lu pcm_rms=%.1f samples=%lu",
                     static_cast<unsigned long>(codec_->source_peak()),
                     static_cast<unsigned long>(codec_->pcm_peak()),
                     static_cast<double>(codec_->pcm_rms()),
                     static_cast<unsigned long>(codec_->pcm_samples()));
        } else if (strcmp(command, "wifi-config") == 0) {
            EnterWifiConfigMode();
        } else if (strcmp(command, "environment") == 0) {
            ESP_LOGI(TAG, "environment=%s", stm32_.EnvironmentJson().c_str());
        } else if (strcmp(command, "robot-status") == 0) {
            ESP_LOGI(TAG, "robot=%s", stm32_.StatusJson().c_str());
        } else if (strcmp(command, "display-status") == 0) {
            ESP_LOGI(TAG, "display=%s", display_->StatusJson().c_str());
        } else if (strncmp(command, "expression ", 11) == 0) {
            ESP_LOGI(TAG, "expression_request=%d", display_->ShowExpression(command + 11, true));
        } else if (strncmp(command, "action ", 7) == 0) {
            ESP_LOGI(TAG, "action=%s", stm32_.Action(command + 7).c_str());
        } else {
            int volume = 0;
            char trailing;
            if (sscanf(command, "volume %d %c", &volume, &trailing) == 1 &&
                volume >= 0 && volume <= 100) {
                codec_->SetOutputVolume(volume);
            } else {
                ESP_LOGW(TAG, "Unknown command");
            }
        }
    }

public:
    RobotDogBoard() {
        // The voice build owns the CH340 console. The old UART bridge is not running.
        uart_config_t serial_config = {};
        serial_config.baud_rate = 115200;
        serial_config.data_bits = UART_DATA_8_BITS;
        serial_config.parity = UART_PARITY_DISABLE;
        serial_config.stop_bits = UART_STOP_BITS_1;
        serial_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        serial_config.source_clk = UART_SCLK_DEFAULT;
        ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &serial_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, GPIO_NUM_43, GPIO_NUM_44,
                                    UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 512, 0, 0, nullptr, 0));

        // WifiStation already sorts scanned APs by RSSI. Honour that AP's
        // BSSID/channel instead of letting a second SSID-only scan join a
        // much weaker same-name AP. Rescans still select a new best AP.
        {
            Settings wifi("wifi", true);
            if (!wifi.GetBool("remember_bssid", false)) {
                wifi.SetBool("remember_bssid", true);
            }
        }
        ESP_ERROR_CHECK(BoardBus_Init());
        bus_ = BoardBus_Handle();
        codec_ = new RobotDogAudioCodec(bus_);
        display_ = new RobotDisplay();
        auto stm_error = stm32_.Start();
        if (stm_error != ESP_OK) ESP_LOGE(TAG, "STM32 link: %s", esp_err_to_name(stm_error));
        InitializeTools();

#if defined(ROBOT_WIFI_SSID) && defined(ROBOT_WIFI_PASSWORD)
        auto& ssids = SsidManager::GetInstance();
        if (ssids.GetSsidList().empty() && strlen(ROBOT_WIFI_SSID) > 0) {
            ssids.AddSsid(ROBOT_WIFI_SSID, ROBOT_WIFI_PASSWORD);
            ESP_LOGI(TAG, "Initial WiFi profile saved");
        }
#endif
        BaseType_t task_ok = xTaskCreate([](void* arg) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            static_cast<RobotDogBoard*>(arg)->ConsoleTask();
        }, "voice_console", 4096, this, 2, nullptr);
        ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_LOGI(TAG, "Voice peripherals ready; screen expressions / STM32 sensors and bounded motion tools; camera disabled");
    }

    AudioCodec* GetAudioCodec() override { return codec_; }
    Display* GetDisplay() override { return display_; }
    std::string GetDeviceStatusJson() override {
        auto root = cJSON_Parse(WifiBoard::GetDeviceStatusJson().c_str());
        if (!root) return "{}";
        cJSON_AddItemToObject(root, "environment", cJSON_Parse(stm32_.EnvironmentJson().c_str()));
        cJSON_AddItemToObject(root, "robot", cJSON_Parse(stm32_.StatusJson().c_str()));
        cJSON_DeleteItemFromObject(root, "screen");
        cJSON_AddItemToObject(root, "screen", cJSON_Parse(display_->StatusJson().c_str()));
        char* json = cJSON_PrintUnformatted(root);
        std::string result = json ? json : "{}";
        cJSON_free(json);
        cJSON_Delete(root);
        return result;
    }
};

DECLARE_BOARD(RobotDogBoard);
