#include "wifi_board.h"
#include "robot_audio_codec.h"
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

    void ConsoleTask() {
        char line[80] = {};
        size_t used = 0;
        bool overflow = false;
        ESP_LOGI(TAG, "Console: chat | status | volume 0..100 | wifi-config");
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
        // WifiStation already sorts scanned APs by RSSI. Honour that AP's
        // BSSID/channel instead of letting a second SSID-only scan join a
        // much weaker same-name AP. Rescans still select a new best AP.
        {
            Settings wifi("wifi", true);
            if (!wifi.GetBool("remember_bssid", false)) {
                wifi.SetBool("remember_bssid", true);
            }
        }
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = GPIO_NUM_8;
        bus_config.scl_io_num = GPIO_NUM_19;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_));
        codec_ = new RobotDogAudioCodec(bus_);

#if defined(ROBOT_WIFI_SSID) && defined(ROBOT_WIFI_PASSWORD)
        auto& ssids = SsidManager::GetInstance();
        if (ssids.GetSsidList().empty() && strlen(ROBOT_WIFI_SSID) > 0) {
            ssids.AddSsid(ROBOT_WIFI_SSID, ROBOT_WIFI_PASSWORD);
            ESP_LOGI(TAG, "Initial WiFi profile saved");
        }
#endif
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
        BaseType_t task_ok = xTaskCreate([](void* arg) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            static_cast<RobotDogBoard*>(arg)->ConsoleTask();
        }, "voice_console", 4096, this, 2, nullptr);
        ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_LOGI(TAG, "Voice board ready; no screen or camera required, no STM32 actuator commands");
    }

    AudioCodec* GetAudioCodec() override { return codec_; }
};

DECLARE_BOARD(RobotDogBoard);
