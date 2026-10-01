#pragma once
#include "espnow_service.h"
#include "esp_err.h"

namespace EmergencyRemote {
enum class State : uint8_t {
    READY, OFF, ON, STOPPING, STARTING, SHORT_CHECK, SHORT_FAULT, PROTECTED,
    OFFLINE, UNPAIRED, REJECTED, COOLDOWN, BUSY, NOT_READY, DETECT_ERROR
};
struct Snapshot {
    State state = State::UNPAIRED;
    bool stop_closed = false;
    bool paired = false;
    bool online = false;
    bool output_on = false;
    bool busy = true;
    bool pairing = false;
    bool quiesced = false;
    bool connection_failed = false;
    int64_t output_time_us = 0;
    uint32_t on_attempt = 0; // Advances for each submitted ON, including attempts completed between UI frames.
    uint8_t protection_mask = 0; // OTP=1, OVP=2, UVP=4, OCP=8
    EspNowService::DeviceData data{};
    int64_t data_time_us = 0;
};
esp_err_t init(bool resume_release = false);
void stop_from_isr();
void request_stop();
void request_on(); // Diagnostic command; ignored while the physical contact is closed.
void start_pairing(bool clear_first = false);
bool prepare_sleep(); // Worker handshake; refuses queued control or unknown/ON output.
void cancel_sleep();
void retry_connection();
void report_battery(uint8_t percent);
void test_wrong_channel(); // Diagnostic: move to channel 6; pending OFF must recover without another press.
Snapshot snapshot();
}
