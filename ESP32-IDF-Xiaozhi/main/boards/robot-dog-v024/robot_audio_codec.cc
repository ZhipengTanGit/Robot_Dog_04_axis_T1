#include "robot_audio_codec.h"
#include "settings.h"
#include "Bsp_BoardBus.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/task.h>

namespace {
constexpr char kTag[] = "RobotDogAudio";
constexpr int kRate = 16000;
constexpr size_t kFrames = 240;
constexpr uint8_t kAmpShutdown = 0x10; // XL9555 P04: HIGH = shutdown.
constexpr int kDefaultVolume = 30;
// Voice playback: unity DAC gain instead of the short-tone diagnostic's -12 dB.
// LOUT2 is also unity gain; software volume remains user-adjustable.
constexpr uint8_t kLeftDacVolume = 0x00;
constexpr uint8_t kLeftOutputVolume = 0x1e; // ES8388 Rev10: -45 + 1.5 * 30 = 0 dB.
constexpr int kDmaDrainMs = 110; // Six 15 ms DMA descriptors plus margin.
}

RobotDogAudioCodec::RobotDogAudioCodec(i2c_master_bus_handle_t bus) {
    duplex_ = true;
    input_reference_ = false; // Right ADC is unpopulated, not an AEC reference.
    input_sample_rate_ = output_sample_rate_ = kRate;
    input_channels_ = output_channels_ = 1;
    input_gain_ = 18.0f;
    output_volume_ = kDefaultVolume;

    const auto error = Initialize(bus);
    StopDiagnosticClock();
    last_error_.store(error);
    ready_.store(error == ESP_OK);
    if (error != ESP_OK) {
        ReportError("initialize", error);
        (void)ShutdownSpeaker();
    } else {
        ESP_LOGI(kTag, "ES8388 verified at 50 kHz; LIN1 / LOUT2, 16000 Hz mono, I2S stereo left slot");
    }
}

RobotDogAudioCodec::~RobotDogAudioCodec() {
    // AudioService must be stopped first; these locks also finish current I/O.
    std::scoped_lock locks(read_mutex_, write_mutex_, control_mutex_);
    started_.store(false);
    input_active_.store(false);
    output_active_.store(false);
    (void)ShutdownSpeaker();
    if (rx_running_) (void)i2s_channel_disable(rx_handle_);
    if (tx_running_) (void)i2s_channel_disable(tx_handle_);
    if (rx_handle_) (void)i2s_del_channel(rx_handle_);
    if (tx_handle_) (void)i2s_del_channel(tx_handle_);
    StopDiagnosticClock();
    if (codec_) (void)i2c_master_bus_rm_device(codec_);
    if (expander_) (void)i2c_master_bus_rm_device(expander_);
}

void RobotDogAudioCodec::ReportError(const char* operation, esp_err_t error) {
    last_error_.store(error);
    ESP_LOGE(kTag, "%s: %s", operation, esp_err_to_name(error));
}

esp_err_t RobotDogAudioCodec::ReadRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t& value) {
    if (!device) return ESP_ERR_INVALID_STATE;
    return BoardBus_Read(device, &reg, 1, &value, 1);
}

esp_err_t RobotDogAudioCodec::WriteRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
    if (!device) return ESP_ERR_INVALID_STATE;
    const uint8_t bytes[] = {reg, value};
    return BoardBus_Write(device, bytes, sizeof(bytes));
}

esp_err_t RobotDogAudioCodec::CheckRegister(uint8_t reg, uint8_t expected, uint8_t mask) {
    uint8_t value = 0;
    auto error = ReadRegister(codec_, reg, value);
    if (error == ESP_OK && (value & mask) != (expected & mask)) {
        ESP_LOGE(kTag, "ES8388 reg 0x%02x: got 0x%02x expected 0x%02x mask 0x%02x", reg, value, expected, mask);
        error = ESP_ERR_INVALID_RESPONSE;
    }
    if (error != ESP_OK) ESP_LOGE(kTag, "ES8388 readback 0x%02x failed", reg);
    return error;
}

esp_err_t RobotDogAudioCodec::UpdateExpander(uint8_t reg, uint8_t mask, uint8_t value) {
    return BoardBus_XlUpdate(reg, mask, value);
}

esp_err_t RobotDogAudioCodec::SetMute(bool mute) {
    const uint8_t value = mute ? 0x26 : 0x22; // DAC mute bit + soft ramp.
    auto error = WriteRegister(codec_, 0x19, value);
    return error == ESP_OK ? CheckRegister(0x19, value, 0xec) : error;
}

esp_err_t RobotDogAudioCodec::ShutdownSpeaker() {
    // Always try both controls, including when one I2C transaction fails.
    const auto amp_error = UpdateExpander(2, kAmpShutdown, kAmpShutdown);
    const auto mute_error = SetMute(true);
    return amp_error != ESP_OK ? amp_error : mute_error;
}

esp_err_t RobotDogAudioCodec::Initialize(i2c_master_bus_handle_t bus) {
    if (!bus) return ESP_ERR_INVALID_ARG;
    // Never drive BCLK/LRCK while the codec still has its reset master setting.
    for (gpio_num_t pin : {GPIO_NUM_7, GPIO_NUM_15, GPIO_NUM_16}) {
        const auto error = gpio_set_direction(pin, GPIO_MODE_INPUT);
        if (error != ESP_OK) return error;
    }
    i2c_device_config_t device = {};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = 0x20;
    device.scl_speed_hz = 100000;
    auto error = i2c_master_bus_add_device(bus, &device, &expander_);
    if (error != ESP_OK) return error;
    // Preload P04 high before making it output. Preserve camera/display bits.
    for (unsigned attempt=0;attempt<4;++attempt) {
        error = UpdateExpander(2, kAmpShutdown, kAmpShutdown);
        if (error == ESP_OK) error = UpdateExpander(6, kAmpShutdown, 0);
        if (error == ESP_OK) break;
        ESP_LOGW(kTag,"Amplifier preload/config readback retry %u: %s",attempt+1,esp_err_to_name(error));
        if (attempt<3) vTaskDelay(pdMS_TO_TICKS(100U*(attempt+1U)));
    }
    if (error != ESP_OK) return error;

    device.device_address = 0x10;
    device.scl_speed_hz = 50000;
    error = i2c_master_bus_add_device(bus, &device, &codec_);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(200));
    uint8_t value = 0;
    error = ReadRegister(codec_, 0x00, value);
    if (error == ESP_OK) error = ReadRegister(codec_, 0x08, value);
    // IDF's address-only probe is fixed at 100 kHz and falsely NACKs on this
    // board. Reachability is determined by these actual 50 kHz register reads.
    if (error != ESP_OK) {
        error = StartDiagnosticClock();
        if (error != ESP_OK) return error;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        error = ConfigureCodec();
        if (error == ESP_OK) break;
        if (attempt != 2) vTaskDelay(pdMS_TO_TICKS((attempt + 1) * 500));
    }
    StopDiagnosticClock();
    if (error == ESP_OK) error = CreateChannels();
    if (error == ESP_OK) error = ShutdownSpeaker();
    return error;
}

esp_err_t RobotDogAudioCodec::ConfigureCodec() {
    auto error = WriteRegister(codec_, 0x00, 0x80);
    if (error != ESP_OK) return error;
    vTaskDelay(pdMS_TO_TICKS(10));
    // Board-proven sequence: release SCPReset first. Only LIN1 / LOUT2 are
    // populated, right input/ADC and MICBIAS are off, no analog feedback path.
    static constexpr uint8_t config[][2] = {
        {0x00, 0x16}, {0x19, 0x26}, {0x01, 0x50}, {0x02, 0x00},
        {0x03, 0x58}, {0x04, 0x48}, {0x05, 0x00}, {0x06, 0x00}, {0x08, 0x00},
        {0x09, 0x60}, {0x0a, 0x00}, {0x0b, 0x02}, {0x0c, 0x0c}, {0x0d, 0x02},
        {0x0e, 0x30}, {0x0f, 0x20}, {0x10, 0x00}, {0x11, 0xc0}, {0x12, 0x38},
        {0x17, 0x18}, {0x18, 0x02}, {0x1a, kLeftDacVolume}, {0x1b, 0xc0},
        {0x26, 0x00}, {0x27, 0xb8}, {0x2a, 0x38}, {0x2b, 0x80},
        {0x2e, 0x00}, {0x2f, 0x00}, {0x30, kLeftOutputVolume}, {0x31, 0x00},
    };
    for (const auto& reg : config) {
        error = WriteRegister(codec_, reg[0], reg[1]);
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "ES8388 write 0x%02x failed", reg[0]);
            return error;
        }
    }
    // Same documented masks as the tested diagnostic firmware. These cover
    // clock/format, actual mute, LIN1 fixed +18 dB PGA, ADC routing and ALC off.
    static constexpr uint8_t checks[][3] = {
        {0x08, 0x00, 0xff}, {0x0a, 0x00, 0xfc}, {0x0c, 0x0c, 0xff},
        {0x17, 0x18, 0xfe}, {0x19, 0x26, 0xec}, {0x30, kLeftOutputVolume, 0x3f},
        {0x1a, kLeftDacVolume, 0xff},
        {0x03, 0x58, 0xff}, {0x09, 0x60, 0xff}, {0x0b, 0x02, 0x9c},
        {0x0d, 0x02, 0x3f}, {0x0e, 0x30, 0xf0}, {0x0f, 0x20, 0xec},
        {0x10, 0x00, 0xff}, {0x11, 0xc0, 0xff}, {0x12, 0x38, 0xff},
    };
    for (const auto& reg : checks) {
        error = CheckRegister(reg[0], reg[1], reg[2]);
        if (error != ESP_OK) return error;
    }
    return ESP_OK;
}

esp_err_t RobotDogAudioCodec::StartDiagnosticClock() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_3;
    timer.duty_resolution = LEDC_TIMER_1_BIT;
    timer.freq_hz = kRate * 256;
    timer.clk_cfg = LEDC_AUTO_CLK;
    auto error = ledc_timer_config(&timer);
    if (error != ESP_OK) return error;
    diagnostic_timer_ = true;
    ledc_channel_config_t channel = {};
    channel.gpio_num = GPIO_NUM_6;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_7;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = LEDC_TIMER_3;
    channel.duty = 1;
    error = ledc_channel_config(&channel);
    diagnostic_channel_ = error == ESP_OK;
    return error;
}

void RobotDogAudioCodec::StopDiagnosticClock() {
    if (diagnostic_channel_) (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
    if (diagnostic_timer_) (void)ledc_timer_pause(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3);
    diagnostic_channel_ = diagnostic_timer_ = false;
}

esp_err_t RobotDogAudioCodec::CreateChannels() {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = kFrames;
    channel.auto_clear_after_cb = true; // Underflow sends silence, never repeats speech.
    auto error = i2s_new_channel(&channel, &tx_handle_, &rx_handle_);
    if (error != ESP_OK) return error;
    i2s_std_config_t config = {};
    config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk = GPIO_NUM_6;
    config.gpio_cfg.bclk = GPIO_NUM_7;
    config.gpio_cfg.ws = GPIO_NUM_15;
    config.gpio_cfg.dout = GPIO_NUM_16;
    config.gpio_cfg.din = GPIO_NUM_17;
    error = i2s_channel_init_std_mode(tx_handle_, &config);
    if (error == ESP_OK) error = i2s_channel_init_std_mode(rx_handle_, &config);
    return error;
}

void RobotDogAudioCodec::Start() {
    if (!ready_.load() || started_.load()) return;
    {
        Settings settings("audio", false);
        output_volume_ = std::clamp(static_cast<int>(settings.GetInt("output_volume", kDefaultVolume)), 0, 100);
        pcm_volume_.store(output_volume_);
    }
    std::scoped_lock locks(read_mutex_, write_mutex_, control_mutex_);
    auto error = ShutdownSpeaker();
    if (error == ESP_OK) error = i2s_channel_enable(tx_handle_);
    tx_running_ = error == ESP_OK;
    if (error == ESP_OK) error = i2s_channel_enable(rx_handle_);
    rx_running_ = error == ESP_OK;
    if (error != ESP_OK) {
        (void)ShutdownSpeaker();
        if (tx_running_) (void)i2s_channel_disable(tx_handle_);
        tx_running_ = false;
        ready_.store(false);
        ReportError("start", error);
        return;
    }
    started_.store(true);
    input_active_.store(true);
    input_enabled_ = true;
    // Open the amplifier only when AudioService actually requests playback.
    output_enabled_ = false;
    ESP_LOGI(kTag, "Audio started; PGA +18 dB, DAC 0 dB, LOUT2 0 dB, output volume %d%%", output_volume_);
}

void RobotDogAudioCodec::SetOutputVolume(int volume) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    volume = std::clamp(volume, 0, 100);
    pcm_volume_.store(volume);
    // Keep the voice DAC / LOUT2 attenuation fixed; scale PCM without clipping.
    AudioCodec::SetOutputVolume(volume);
}

void RobotDogAudioCodec::SetInputGain(float gain) {
    // Do not silently change the verified LIN1 gain/routing for a generic API.
    if (gain != 18.0f) ESP_LOGW(kTag, "Requested PGA %.1f dB; board keeps verified +18 dB", gain);
    input_gain_ = 18.0f;
}

void RobotDogAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(read_mutex_);
    const bool active = enable && started_.load() && ready_.load() && !input_fault_.load();
    input_active_.store(active);
    AudioCodec::EnableInput(active);
    // Keep full-duplex clocks running; disabling RX clocks would disrupt TX.
}

void RobotDogAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (enable && (!started_.load() || !ready_.load() || output_fault_.load())) return;
    if (enable == output_active_.load()) return;
    auto error = ESP_OK;
    if (enable) {
        // Queue silence with the amp still off, then outwait every old DMA
        // descriptor. This also avoids replaying any queued pre-stop speech.
        std::array<int16_t, kFrames * 2> zeros = {};
        size_t sent = 0;
        error = Transfer(true, zeros.data(), sizeof(zeros), sent);
        if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(kDmaDrainMs));
        if (error == ESP_OK) {
            std::lock_guard<std::mutex> control(control_mutex_);
            error = SetMute(false);
            if (error == ESP_OK) error = UpdateExpander(2, kAmpShutdown, 0);
        }
    } else {
        std::lock_guard<std::mutex> control(control_mutex_);
        error = ShutdownSpeaker();
    }
    if (error != ESP_OK) {
        StopOutputOnError(enable ? "enable speaker" : "disable speaker", error);
        return;
    }
    output_active_.store(enable);
    AudioCodec::EnableOutput(enable);
}

void RobotDogAudioCodec::StopOutputOnError(const char* operation, esp_err_t error) {
    ReportError(operation, error);
    output_fault_.store(true); // Recovery requires restart, never pretend playback worked.
    output_active_.store(false);
    AudioCodec::EnableOutput(false);
    {
        std::lock_guard<std::mutex> control(control_mutex_);
        const auto shutdown = ShutdownSpeaker();
        if (shutdown != ESP_OK) ESP_LOGE(kTag, "Speaker shutdown NOT confirmed: %s", esp_err_to_name(shutdown));
    }
    if (tx_running_) {
        const auto stopped = i2s_channel_disable(tx_handle_);
        if (stopped == ESP_OK) tx_running_ = false;
        else ESP_LOGE(kTag, "TX disable failed: %s", esp_err_to_name(stopped));
    }
}

esp_err_t RobotDogAudioCodec::Transfer(bool write, void* data, size_t size, size_t& transferred) {
    transferred = 0;
    const int64_t deadline = esp_timer_get_time() + 300000;
    auto* bytes = static_cast<uint8_t*>(data);
    while (transferred < size) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        size_t count = 0;
        const auto error = write
            ? i2s_channel_write(tx_handle_, bytes + transferred, size - transferred, &count, 100)
            : i2s_channel_read(rx_handle_, bytes + transferred, size - transferred, &count, 100);
        if (count > size - transferred) return ESP_ERR_INVALID_SIZE;
        transferred += count;
        if (error != ESP_OK && error != ESP_ERR_TIMEOUT) return error;
        if (transferred == size) return ESP_OK;
        if (count == 0) return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
        // Short I/O resumes at the exact byte offset, even within a stereo frame.
    }
    return ESP_OK;
}

int RobotDogAudioCodec::Read(int16_t* dest, int samples) {
    if (!dest || samples <= 0) return 0;
    std::lock_guard<std::mutex> lock(read_mutex_);
    if (!input_active_.load() || !started_.load()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return 0;
    }
    std::array<int16_t, kFrames * 2> stereo;
    int done = 0;
    while (done < samples) {
        const size_t frames = std::min<size_t>(samples - done, kFrames);
        size_t received = 0;
        const auto error = Transfer(false, stereo.data(), frames * 4, received);
        if (error != ESP_OK) {
            last_error_.store(error);
            if (received % 4 != 0) {
                // A failed transfer may leave the stream halfway through a
                // stereo frame. Do not reinterpret its right slot as LIN1.
                input_fault_.store(true);
                input_active_.store(false);
                AudioCodec::EnableInput(false);
                ESP_LOGE(kTag, "Microphone frame alignment lost; restart required");
            }
            const int64_t now = esp_timer_get_time();
            if (last_read_error_us_ == 0 || now - last_read_error_us_ >= 1000000) {
                ReportError("microphone read", error);
                last_read_error_us_ = now;
            }
            return done;
        }
        for (size_t i = 0; i < frames; ++i) dest[done + i] = stereo[2 * i];
        done += frames;
    }
    return done;
}

int RobotDogAudioCodec::Write(const int16_t* data, int samples) {
    if (!data || samples <= 0) return 0;
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!output_active_.load() || !started_.load() || output_fault_.load()) return 0;
    std::array<int16_t, kFrames * 2> stereo = {};
    const int volume = pcm_volume_.load();
    uint64_t accepted_samples = 0, sum_squares = 0;
    uint32_t block_peak = 0, block_source_peak = 0;
    const auto publish_statistics = [&] {
        if (!accepted_samples) return;
        const uint64_t total = pcm_samples_.load(std::memory_order_relaxed) + accepted_samples;
        pcm_samples_.store(static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX)),
                           std::memory_order_relaxed);
        // write_mutex_ makes this the sole writer of the running maxima.
        if (block_peak > pcm_peak_.load(std::memory_order_relaxed))
            pcm_peak_.store(block_peak, std::memory_order_relaxed);
        if (block_source_peak > source_peak_.load(std::memory_order_relaxed))
            source_peak_.store(block_source_peak, std::memory_order_relaxed);
        if (sum_squares) {
            const float rms = static_cast<float>(std::sqrt(
                static_cast<double>(sum_squares) / accepted_samples));
            pcm_rms_.store(rms, std::memory_order_relaxed);
        }
    };
    int done = 0;
    while (done < samples) {
        const size_t frames = std::min<size_t>(samples - done, kFrames);
        for (size_t i = 0; i < frames; ++i) {
            stereo[2 * i] = static_cast<int16_t>(static_cast<int32_t>(data[done + i]) * volume / 100);
            stereo[2 * i + 1] = 0;
        }
        size_t sent = 0;
        const auto error = Transfer(true, stereo.data(), frames * 4, sent);
        // Count only complete accepted frames, including a successful prefix
        // of a failed write. Do not measure queued-but-unwritten source data.
        const size_t accepted_frames = sent / 4;
        for (size_t i = 0; i < accepted_frames; ++i) {
            const int64_t value = stereo[2 * i];
            const int32_t source = data[done + i];
            const uint32_t absolute = static_cast<uint32_t>(value < 0 ? -value : value);
            const uint32_t source_absolute = static_cast<uint32_t>(source < 0 ? -source : source);
            block_peak = std::max(block_peak, absolute);
            block_source_peak = std::max(block_source_peak, source_absolute);
            sum_squares += static_cast<uint64_t>(value * value);
        }
        accepted_samples += accepted_frames;
        if (error != ESP_OK) {
            publish_statistics();
            StopOutputOnError("speaker write", error);
            return done + sent / 4;
        }
        done += frames;
    }
    publish_statistics();
    return done;
}

bool RobotDogAudioCodec::InputData(std::vector<int16_t>& data) {
    if (data.empty() || data.size() > INT_MAX) return false;
    if (Read(data.data(), static_cast<int>(data.size())) == static_cast<int>(data.size())) return true;
    // Base InputData accepts any positive short read; speech processing needs
    // a complete frame. Never report success with stale/uninitialized samples.
    std::fill(data.begin(), data.end(), 0);
    return false;
}

void RobotDogAudioCodec::OutputData(std::vector<int16_t>& data) {
    if (data.empty() || data.size() > INT_MAX) return;
    const int written = Write(data.data(), static_cast<int>(data.size()));
    if (written != static_cast<int>(data.size()) && !output_fault_.load()) {
        ESP_LOGW(kTag, "Playback not accepted (%d/%u samples)", written, static_cast<unsigned>(data.size()));
    }
}
