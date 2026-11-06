#pragma once

#include "audio_codec.h"

#include <driver/i2c_master.h>
#include <atomic>
#include <mutex>

// The board owns the GPIO8/19 I2C0 bus and must outlive this codec. This codec
// exclusively owns its ES8388/XL9555 handles, I2S0 and LEDC timer3/channel7.
// Stop AudioService before destroying it; do not start the old BSP audio task.
class RobotDogAudioCodec final : public AudioCodec {
public:
    explicit RobotDogAudioCodec(i2c_master_bus_handle_t bus);
    ~RobotDogAudioCodec() override;

    bool ready() const { return ready_.load(); }
    esp_err_t last_error() const { return last_error_.load(); }
    // PCM statistics count complete stereo frames accepted by I2S, expressed
    // as mono left-channel samples. They are digital levels, not speaker SPL.
    uint32_t pcm_peak() const { return pcm_peak_.load(std::memory_order_relaxed); }
    float pcm_rms() const { return pcm_rms_.load(std::memory_order_relaxed); }
    // Saturates at UINT32_MAX (about 74.6 hours of continuous 16 kHz PCM).
    uint64_t pcm_samples() const { return pcm_samples_.load(std::memory_order_relaxed); }
    uint32_t source_peak() const { return source_peak_.load(std::memory_order_relaxed); }

    void Start() override;
    void SetOutputVolume(int volume) override;
    void SetInputGain(float gain) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
    bool InputData(std::vector<int16_t>& data) override;
    void OutputData(std::vector<int16_t>& data) override;

protected:
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;

private:
    i2c_master_dev_handle_t codec_ = nullptr;
    i2c_master_dev_handle_t expander_ = nullptr;
    std::mutex control_mutex_;
    std::mutex read_mutex_;
    std::mutex write_mutex_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> input_active_{false};
    std::atomic<bool> input_fault_{false};
    std::atomic<bool> output_active_{false};
    std::atomic<bool> output_fault_{false};
    std::atomic<int> pcm_volume_{30};
    std::atomic<esp_err_t> last_error_{ESP_ERR_INVALID_STATE};
    std::atomic<uint32_t> pcm_peak_{0};
    std::atomic<float> pcm_rms_{0.0f}; // Latest nonzero Write block; silence preserves it.
    std::atomic<uint32_t> pcm_samples_{0};
    std::atomic<uint32_t> source_peak_{0};
    bool tx_running_ = false;
    bool rx_running_ = false;
    bool diagnostic_timer_ = false;
    bool diagnostic_channel_ = false;
    int64_t last_read_error_us_ = 0;

    esp_err_t Initialize(i2c_master_bus_handle_t bus);
    esp_err_t ConfigureCodec();
    esp_err_t CreateChannels();
    esp_err_t StartDiagnosticClock();
    void StopDiagnosticClock();
    esp_err_t ReadRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t& value);
    esp_err_t WriteRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value);
    esp_err_t CheckRegister(uint8_t reg, uint8_t value, uint8_t mask = 0xff);
    esp_err_t UpdateExpander(uint8_t reg, uint8_t mask, uint8_t value);
    esp_err_t SetMute(bool mute);
    esp_err_t ShutdownSpeaker();  // Caller owns control_mutex_.
    void StopOutputOnError(const char* operation, esp_err_t error); // Own write_mutex_.
    esp_err_t Transfer(bool write, void* data, size_t size, size_t& transferred);
    void ReportError(const char* operation, esp_err_t error);
};
