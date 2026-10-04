/**
 * @file battery_monitor.cpp
 * @brief 产品电池策略实现：结果邮箱、采样重试、USB 校准启动与连接电量上报。
 */
#include "battery_monitor.h"

#include "app_controller.h"
#include "battery_status.h"
#include "battery_voltage.h"
#include "diagnostic_log.h"
#include "esp_log.h"
#include "hardware.h"
#include "runtime_settings.h"

namespace AppController {
namespace {
constexpr char kTag[] = "BatteryMonitor";
constexpr int64_t kBootRetryUs = 1000000; // 首次有效采样取得前，失败重试间隔为 1 秒。
} // namespace

esp_err_t BatteryMonitor::init() {
    if (samples_ != nullptr)
        return ESP_ERR_INVALID_STATE;
    // 邮箱必须先于异步采样建立，保证快速完成的回调也有有效发布目标。
    samples_ = xQueueCreate(1, sizeof(Sample));
    if (samples_ == nullptr)
        return ESP_ERR_NO_MEM;
    request_sample();
    return ESP_OK;
}

void BatteryMonitor::on_sample(esp_err_t result, int voltage_mv, void *context) {
    auto &monitor = *static_cast<BatteryMonitor *>(context);
    const Sample sample{result, voltage_mv};
    // 回调运行在采样任务中；不写 UI、业务状态或日志，也不等待消费者。
    (void)xQueueOverwrite(monitor.samples_, &sample);
}

void BatteryMonitor::request_sample() {
    const esp_err_t result = BatteryVoltage::start_async(on_sample, this);
    // 启动错误和后台采样错误走同一个消费路径，调用方无需同步等待或更新 UI。
    if (result != ESP_OK)
        on_sample(result, 0, this);
}

void BatteryMonitor::begin(int64_t now_us, bool usb) {
    sample_at_ = now_us + RuntimeSettings::get(RuntimeSettings::Id::BatteryMs) * 1000LL;
    usb_before_ = usb;
    BatteryVoltage::notify_external_power(usb);
    if (usb)
        (void)BatteryVoltage::start_calibration_monitor(Hardware::usb_connected);
}

BatteryMonitor::Update BatteryMonitor::poll(int64_t now_us, bool usb) {
    Update update;
    Sample sample{};
    if (xQueueReceive(samples_, &sample, 0) == pdTRUE) {
        if (sample.result == ESP_OK) {
            latest_mv_ = sample.voltage_mv;
            const auto level = BatteryStatus::update(sample.voltage_mv, usb);
            // 启动诊断不等待电池；首次成功在这里补记一次产品事件。
            if (boot_pending_) {
                DEVICE_EVENT_I(kEventTag, "boot battery ready battery_mv=%d soc=%u", sample.voltage_mv,
                               static_cast<unsigned>(level.displayed_percent));
                boot_pending_ = false;
            }
            update.redraw = true;
        } else {
            ESP_LOGE(kTag, "battery sample: %s", esp_err_to_name(sample.result));
            if (boot_pending_)
                sample_at_ = now_us + kBootRetryUs;
        }
    }

    // 只有 USB 边沿计为活动；常规采样和电量变化不会延后静置休眠。
    if (usb != usb_before_) {
        const auto battery = view();
        DEVICE_EVENT_I(kEventTag, "USB %s battery_mv=%d soc=%d", usb ? "inserted" : "removed",
                       battery.voltage_mv, battery.percent);
        usb_before_ = usb;
    BatteryVoltage::notify_external_power(usb);
        sample_at_ = now_us;
        update.activity = true;
        update.redraw = true;
        // 只在插入时启动；拔出后的退出由校准任务通过 USB 查询回调处理。
        if (usb && BatteryVoltage::start_calibration_monitor(Hardware::usb_connected) == ESP_OK)
            ESP_LOGI(kTag, "battery calibration monitor started");
    }

    if (now_us >= sample_at_) {
        if (!BatteryVoltage::is_busy())
            request_sample();
        // 忙时也顺延到下一个周期，不在每个协调循环内重复尝试创建采样任务。
        sample_at_ = now_us + RuntimeSettings::get(RuntimeSettings::Id::BatteryMs) * 1000LL;
        update.redraw = true;
    }
    return update;
}

BatteryView BatteryMonitor::view() const {
    BatteryLevel::Status status{};
    if (BatteryStatus::get_status(status))
        return {status.voltage_mv, status.displayed_percent};
    return {latest_mv_, -1};
}

void BatteryMonitor::report(const EmergencyRemote::Snapshot &remote, int percent, int64_t now_us) {
    // 上报条件比日志的原始 online 更严格，因此独立维护 connected_before_。
    const bool connected = remote.online && !remote.connection_failed;
    if (connected && percent >= 0 && (now_us >= report_at_ || !connected_before_)) {
        EmergencyRemote::report_battery(static_cast<uint8_t>(percent));
        report_at_ = now_us + RuntimeSettings::get(RuntimeSettings::Id::ReportMs) * 1000LL;
    }
    connected_before_ = connected;
}
} // namespace AppController
