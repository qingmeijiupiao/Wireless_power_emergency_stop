/**
 * @file hardware.cpp
 * @brief 板级 GPIO 初始化、按键/急停中断，以及休眠隔离与唤醒恢复的实现。
 */
#include "hardware.h"
#include "hal/usb_serial_jtag_ll.h"
namespace Hardware {
namespace {
// GPIO11 为 VDD_SPI，GPIO12..17 属于 Flash，二者均不可重新配置。
// GPIO3/4/5 必须保持可作为唤醒输入的能力。
// 休眠时置为高阻以降低漏电的引脚集合：GPIO0/1/2/6/7/8/9/10/20/21。
// 其中 GPIO3/4/5（按键、VBUS）以及 USB 引脚不在此集合内，需另行处理。
constexpr uint64_t kSleepIsolatedPins = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 6) | (1ULL << 7) |
                                        (1ULL << 8) | (1ULL << 9) | (1ULL << 10) | (1ULL << 20) | (1ULL << 21);
// 板级相关引脚掩码：取 GPIO0..21，并排除 GPIO11..17（VDD_SPI 与 Flash）。
constexpr uint64_t kBoardPins = ((1ULL << 22) - 1) & ~(((1ULL << 7) - 1) << 11);
// 需要在初始化时主动释放 GPIO 保持的引脚，避免保持状态使后续配置失效。
constexpr gpio_num_t kLegacyHeldPins[] = {GPIO_NUM_0, kScreenEnable, kBatteryDividerEnable};

} // namespace
// 关闭全局深度休眠保持，并逐个解除需要释放的引脚的保持状态。
void release_sleep_holds() {
    gpio_deep_sleep_hold_dis();
    for (const auto pin : kLegacyHeldPins)
        ESP_ERROR_CHECK(gpio_hold_dis(pin));
}

// 休眠中止（未真正入睡）时恢复 USB PHY：Shell 可能正阻塞在 VFS 读取中，
// 因此保留 USB 驱动的队列与状态，直接在底层重新使能 PHY 焊盘，避免卸载活动中的驱动。
void restore_usb_after_sleep_abort() {
    usb_serial_jtag_ll_phy_enable_pad(true);
    usb_serial_jtag_ll_phy_disable_pull_override();
}

// 休眠前隔离 USB PHY 引脚（GPIO18/19）：直接配置焊盘为禁用态，
// gpio_config 同时会关闭 USB PHY 的引脚功能，从而降低休眠电流。
void isolate_usb_for_sleep() {
    gpio_config_t isolated{};
    isolated.pin_bit_mask = (1ULL << GPIO_NUM_18) | (1ULL << GPIO_NUM_19);
    isolated.mode = GPIO_MODE_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&isolated));
}

// 断开电池分压器：同时关闭数字接收器，因为浮空的分压节点可能停在
// 翻转阈值附近；高阻并不等于一个被主动驱动的高电平。
void disconnect_battery_divider() {
    ESP_ERROR_CHECK(gpio_set_direction(kBatteryDividerEnable, GPIO_MODE_DISABLE));
    ESP_ERROR_CHECK(gpio_set_pull_mode(kBatteryDividerEnable, GPIO_FLOATING));
}

// 进入休眠前的 GPIO 配置：先释放保持，再关闭屏幕供电，
// 批量把 kSleepIsolatedPins 中的引脚置为禁用态以降低漏电。
void configure_sleep_gpio() {
    release_sleep_holds();
    ESP_ERROR_CHECK(gpio_set_level(kScreenEnable, 0));
    gpio_config_t isolated{};
    isolated.pin_bit_mask = kSleepIsolatedPins;
    isolated.mode = GPIO_MODE_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&isolated));
    // GPIO6 隔离期间由外部 R11 将 OLED MOS 栅极拉低，因此不使用数字输出保持。
    ESP_ERROR_CHECK(gpio_set_pull_mode(kStopButton, GPIO_FLOATING));
}

// 板级初始化：配置各 GPIO 模式，并注册急停按键的 IRAM 中断处理函数。
void init(gpio_isr_t stop_handler) {
    release_sleep_holds();
    // 用户按键配置为带上拉的输入：松开为高，按下为低。
    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << kUiButton;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    // Q1 用于切换 OLED 的地回路；先输出高电平打开屏幕供电。
    ESP_ERROR_CHECK(gpio_set_level(kScreenEnable, 1));
    // 屏幕使能引脚需要可读回电平，因此配置为输入输出模式。
    gpio_config_t output{};
    output.pin_bit_mask = 1ULL << kScreenEnable;
    output.mode = GPIO_MODE_INPUT_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&output));

    // 急停按键配置为输入，使用外部上拉，下降沿触发中断。
    gpio_config_t stop{};
    stop.pin_bit_mask = 1ULL << kStopButton;
    stop.mode = GPIO_MODE_INPUT;
    stop.pull_up_en = GPIO_PULLUP_DISABLE; // R12 为外部 620k 上拉电阻。
    stop.intr_type = GPIO_INTR_NEGEDGE;
    ESP_ERROR_CHECK(gpio_config(&stop));
    // 中断服务程序需放在 IRAM 中，以便在缓存被禁用时仍能响应急停。
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(kStopButton, stop_handler, nullptr));

    // VBUS 检测为纯输入，用于判断 USB 是否接入。
    gpio_config_t vbus{};
    vbus.pin_bit_mask = 1ULL << kVbusDetect;
    vbus.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&vbus));

    // GPIO10 常态保持高阻，仅在测量电池电压时才被驱动。
    gpio_config_t divider{};
    divider.pin_bit_mask = 1ULL << kBatteryDividerEnable;
    divider.mode = GPIO_MODE_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&divider));
}

// USB 接入判定：VBUS 检测引脚为高即视为已连接。
bool usb_connected() { return gpio_get_level(kVbusDetect) != 0; }
// 用户按键按下判定：低电平有效。
bool button_pressed() { return gpio_get_level(kUiButton) == 0; }
// 急停回路闭合判定：低电平有效。
bool stop_closed() { return gpio_get_level(kStopButton) == 0; }
// 直接写屏幕供电使能引脚电平（true 打开，false 关闭）。
void screen_power(bool enabled) { gpio_set_level(kScreenEnable, enabled); }
// 断开屏幕 I2C 总线：把 SDA/SCL 设为浮空输入，避免总线在休眠时被拉动耗电。
void disconnect_screen_bus() {
    gpio_set_direction(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SDA), GPIO_MODE_INPUT);
    gpio_set_direction(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SCL), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SDA), GPIO_FLOATING);
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SCL), GPIO_FLOATING);
}
// 唤醒后把屏幕供电使能引脚恢复为输入输出模式。
void restore_screen_pin() { ESP_ERROR_CHECK(gpio_set_direction(kScreenEnable, GPIO_MODE_INPUT_OUTPUT)); }
// 打印全部板级引脚（kBoardPins）的 GPIO 配置，便于对照调试。
void dump_gpio() { gpio_dump_io_configuration(stdout, kBoardPins); }
// 打印休眠关键引脚 GPIO0/6/10 的配置，用于核对隔离是否生效。
void dump_sleep_gpio() { gpio_dump_io_configuration(stdout, (1ULL << 0) | (1ULL << 6) | (1ULL << 10)); }
} // namespace Hardware
