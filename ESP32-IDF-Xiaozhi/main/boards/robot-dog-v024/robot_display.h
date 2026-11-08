#pragma once
#include "display.h"
#include <string>
#include <atomic>

class RobotDisplay final : public Display {
public:
    RobotDisplay();
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    bool ShowExpression(const std::string& name, bool manual = false);
    bool Ready();
    std::string StatusJson();
protected:
    bool Lock(int timeout_ms = 0) override { return true; }
    void Unlock() override {}
private:
    std::atomic<uint32_t> manual_until_{0};
};
