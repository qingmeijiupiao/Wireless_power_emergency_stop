#include "emergency_ui.h"
#include "sh1106.h"
#include "pages.h"
#include "product_ui.h"
#include "runtime_policy.h"

#include "esp_sleep.h"
#include "nvs.h"
#include "runtime_settings.h"
#include "battery_level.h"
#include "esp_console.h"
#include <cstdlib>
#include <cstring>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

namespace {
RTC_DATA_ATTR uint32_t sleep_cookie = 0;
RTC_DATA_ATTR bool sleep_contact_low = false;
RTC_DATA_ATTR uint32_t saved_fault = 0, saved_fault_check = 0;
constexpr uint32_t kFaultMagic = 0x46540000;
int last_fault_page = -1;
constexpr uint32_t kSleepCookie = 0x45535450;
int battery_mv = 0, battery_raw = 0, battery_percent = -1;
bool always_on = false;
constexpr gpio_num_t kUiButton = GPIO_NUM_3;
constexpr gpio_num_t kScreenEnable = GPIO_NUM_6;
constexpr gpio_num_t kStopButton = GPIO_NUM_5;
constexpr gpio_num_t kVbusDetect = GPIO_NUM_4;
constexpr gpio_num_t kBatteryDividerEnable = GPIO_NUM_10;
constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_0; // GPIO0, 10k:10k divider
// GPIO11 is VDD_SPI and GPIO12..17 belong to Flash. Do not reconfigure them.
// GPIO3/4/5 must remain usable as wake inputs.
constexpr uint64_t kSleepIsolatedPins = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) |
    (1ULL << 6) | (1ULL << 7) | (1ULL << 8) | (1ULL << 9) | (1ULL << 10) |
    (1ULL << 20) | (1ULL << 21);
constexpr uint64_t kBoardPins = ((1ULL << 22) - 1) & ~(((1ULL << 7) - 1) << 11);
// Release holds left by earlier firmware; this version creates no application holds.
constexpr gpio_num_t kLegacyHeldPins[] = {GPIO_NUM_0, kScreenEnable, kBatteryDividerEnable};

void release_sleep_holds() {
    gpio_deep_sleep_hold_dis();
    for (const auto pin : kLegacyHeldPins) ESP_ERROR_CHECK(gpio_hold_dis(pin));
}

void init_usb_driver() {
    usb_serial_jtag_driver_config_t usb{};
    usb.rx_buffer_size = 256;
    usb.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    // gpio_config(GPIO19) overrides the USB D+ pull-up. Driver installation
    // enables the PHY pads but does not clear that override on ESP32-C3.
    usb_serial_jtag_ll_phy_disable_pull_override();
}

void isolate_usb_for_sleep() {
    // Match the low-power switch: configure the active pads, not just their
    // light-sleep settings. gpio_config also disables the USB PHY pad function.
    gpio_config_t isolated{};
    isolated.pin_bit_mask = (1ULL << GPIO_NUM_18) | (1ULL << GPIO_NUM_19);
    isolated.mode = GPIO_MODE_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&isolated));
}

void disconnect_battery_divider() {
    // Disable the digital receiver too: the floating divider can sit near a
    // switching threshold. High impedance is not an actively driven high level.
    ESP_ERROR_CHECK(gpio_set_direction(kBatteryDividerEnable, GPIO_MODE_DISABLE));
    ESP_ERROR_CHECK(gpio_set_pull_mode(kBatteryDividerEnable, GPIO_FLOATING));
}

void configure_sleep_gpio() {
    release_sleep_holds();
    ESP_ERROR_CHECK(gpio_set_level(kScreenEnable, 0));
    gpio_config_t isolated{};
    isolated.pin_bit_mask = kSleepIsolatedPins;
    isolated.mode = GPIO_MODE_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&isolated));
    // External R11 pulls the OLED MOS gate low while GPIO6 is isolated. Avoid
    // digital output holds, as in the original switch's StatusLed sleep path.
    ESP_ERROR_CHECK(gpio_set_pull_mode(kStopButton, GPIO_FLOATING));
}

void IRAM_ATTR on_stop_fall(void*) {
    EmergencyRemote::stop_from_isr();
}

void init_board_gpio() {
    release_sleep_holds();
    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << kUiButton;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    // Q1 switches OLED GND.
    ESP_ERROR_CHECK(gpio_set_level(kScreenEnable, 1));
    gpio_config_t output{};
    output.pin_bit_mask = 1ULL << kScreenEnable;
    output.mode = GPIO_MODE_INPUT_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&output));

    gpio_config_t stop{};
    stop.pin_bit_mask = 1ULL << kStopButton;
    stop.mode = GPIO_MODE_INPUT;
    stop.pull_up_en = GPIO_PULLUP_DISABLE; // R12 is an external 620k pull-up.
    stop.intr_type = GPIO_INTR_NEGEDGE;
    ESP_ERROR_CHECK(gpio_config(&stop));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(kStopButton, on_stop_fall, nullptr));

    gpio_config_t vbus{};
    vbus.pin_bit_mask = 1ULL << kVbusDetect;
    vbus.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&vbus));

    // GPIO10 is high impedance except during a battery measurement.
    gpio_config_t divider{};
    divider.pin_bit_mask = 1ULL << kBatteryDividerEnable;
    divider.mode = GPIO_MODE_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&divider));
}

adc_oneshot_unit_handle_t init_battery_adc() {
    adc_oneshot_unit_init_cfg_t unit{};
    unit.unit_id = ADC_UNIT_1;
    adc_oneshot_unit_handle_t handle = nullptr;
    esp_err_t err = adc_oneshot_new_unit(&unit, &handle);
    if (err != ESP_OK) {
        ESP_LOGE("emergency_ui", "Battery ADC unit init failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    adc_oneshot_chan_cfg_t channel{};
    channel.atten = ADC_ATTEN_DB_12;
    channel.bitwidth = ADC_BITWIDTH_DEFAULT;
    err = adc_oneshot_config_channel(handle, kBatteryAdcChannel, &channel);
    if (err != ESP_OK) {
        ESP_LOGE("emergency_ui", "Battery ADC channel init failed: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(handle);
        return nullptr;
    }
    return handle;
}

void log_board_snapshot(adc_oneshot_unit_handle_t adc, adc_cali_handle_t calibration) {
    constexpr const char* tag = "emergency_ui";
    ESP_LOGI(tag, "BOARD stop=%s gpio5=%d vbus_gpio4=%d screen_en_gpio6=%d",
        gpio_get_level(kStopButton) == 0 ? "CLOSED/STOP" : "OPEN",
        gpio_get_level(kStopButton), gpio_get_level(kVbusDetect), gpio_get_level(kScreenEnable));
    if (!adc) return;
    ESP_ERROR_CHECK(gpio_set_level(kBatteryDividerEnable, 0));
    ESP_ERROR_CHECK(gpio_set_direction(kBatteryDividerEnable, GPIO_MODE_OUTPUT));
    vTaskDelay(pdMS_TO_TICKS(10));
    int sum = 0, samples = 0;
    for (int i = 0; i < 16; ++i) {
        int raw = 0;
        if (adc_oneshot_read(adc, kBatteryAdcChannel, &raw) == ESP_OK) {
            sum += raw;
            ++samples;
        }
    }
    // Always disconnect the divider after sampling; never leave GPIO10 low.
    disconnect_battery_divider();
    if (!samples) {
        ESP_LOGE(tag, "BATTERY_ADC read failed");
        return;
    }
    const int raw = sum / samples;
    battery_raw = raw;
    int millivolts = 0;
    if (calibration && adc_cali_raw_to_voltage(calibration, raw, &millivolts) == ESP_OK) {
        battery_mv = static_cast<int>(millivolts * 2LL * RuntimeSettings::get("bat_gain_ppm") / 1000000);
        battery_percent = BatteryLevel::update(battery_mv, gpio_get_level(kVbusDetect) != 0).displayed_percent;
        ESP_LOGI(tag, "BATTERY_ADC raw=%d pin_mv=%d battery_mv_approx=%d", raw, millivolts, battery_mv);
    } else {
        ESP_LOGI(tag, "BATTERY_ADC raw=%d calibration_unavailable", raw);
    }
}

RuntimePolicy::SleepBlock sleep_block() {
    const auto s = EmergencyRemote::snapshot();
    return RuntimePolicy::sleep_block(gpio_get_level(kVbusDetect), s.output_on,
        s.connection_failed || (s.output_time_us > 0 && esp_timer_get_time() - s.output_time_us < RuntimeSettings::get("fresh_ms") * 1000LL),
        s.busy || s.pairing, gpio_get_level(kUiButton) == 0);
}

bool save_mode(bool value) {
    nvs_handle_t h;
    if (nvs_open("estop_ui", NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_u8(h, "always_on", value) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) always_on = value;
    return ok;
}

int estop_command(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "config")) RuntimeSettings::print();
    else if (argc == 4 && !strcmp(argv[1], "set")) {
        char* end = nullptr;
        const unsigned long value = strtoul(argv[3], &end, 10);
        if (!*argv[3] || *end || argv[3][0] == '-' || value > UINT32_MAX ||
            !RuntimeSettings::set(argv[2], static_cast<uint32_t>(value))) {
            printf("ERROR invalid name/range or NVS write failure; use estop config\n"); return 1;
        }
        if (!strcmp(argv[2], "bat_gain_ppm")) BatteryLevel::reset();
        printf("SAVED %s=%lu\n", argv[2], value);
    } else if (argc == 2 && !strcmp(argv[1], "retry")) EmergencyRemote::retry_connection();
    else if (argc == 2 && !strcmp(argv[1], "battery"))
        printf("BATTERY raw=%d voltage_mv=%d percent=%d usb=%d low=%d estimate=1\n", battery_raw,
            battery_mv, battery_percent, gpio_get_level(kVbusDetect), battery_mv > 0 && battery_mv <= static_cast<int>(RuntimeSettings::get("low_mv")));
    else if (argc == 2 && !strcmp(argv[1], "gpio"))
        gpio_dump_io_configuration(stdout, kBoardPins);
    else if (argc == 2 && !strcmp(argv[1], "last_fault"))
        printf("LAST_FAULT page=%d (historical reason; not a current output assertion)\n", last_fault_page);
    else if (argc == 2 && !strcmp(argv[1], "status")) {
        const auto s = EmergencyRemote::snapshot();
        printf("STATUS paired=%d online=%d output=%d failed=%d busy=%d usb=%d sleep_block=%u\n", s.paired,
            s.online, s.output_on, s.connection_failed, s.busy, gpio_get_level(kVbusDetect), static_cast<unsigned>(sleep_block()));
    } else { printf("estop config | set <name> <value> | retry | battery | status | gpio\n"); return 1; }
    return 0;
}
void init_console() {
    esp_console_config_t cfg{};
    cfg.max_cmdline_args = 8; cfg.max_cmdline_length = 128;
    ESP_ERROR_CHECK(esp_console_init(&cfg));
    esp_console_cmd_t cmd{};
    cmd.command = "estop"; cmd.help = "Emergency stop settings, battery and connection diagnostics";
    cmd.func = estop_command;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    ESP_ERROR_CHECK(esp_console_register_help_command());
}

int enter_sleep(Sh1106& display, adc_oneshot_unit_handle_t& battery_adc) {
    auto block = sleep_block();
    if (block != RuntimePolicy::SleepBlock::None) return static_cast<int>(block);
    if (!EmergencyRemote::prepare_sleep()) return 4;
    const bool contact_low = gpio_get_level(kStopButton) == 0;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_err_t err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kUiButton, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kVbusDetect, ESP_GPIO_WAKEUP_GPIO_HIGH);
    if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << kStopButton,
        contact_low ? ESP_GPIO_WAKEUP_GPIO_HIGH : ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) { EmergencyRemote::cancel_sleep(); return 7; }
    uint8_t sleep_frame[1024];
    ProductUi::render_message(sleep_frame, 0, always_on);
    const auto sleeping = EmergencyRemote::snapshot();
    ProductUi::render_rail(sleep_frame, sleeping, battery_percent, false, sleeping.output_time_us > 0);
    display.write_frame(sleep_frame, sizeof(sleep_frame));
    display.shutdown();
    gpio_set_direction(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SDA), GPIO_MODE_INPUT);
    gpio_set_direction(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SCL), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SDA), GPIO_FLOATING);
    gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SCL), GPIO_FLOATING);
    disconnect_battery_divider();
    gpio_set_level(kScreenEnable, 0);
    // Recheck after the slow I2C shutdown, while the control worker remains alive.
    block = sleep_block();
    if (block != RuntimePolicy::SleepBlock::None ||
        contact_low != (gpio_get_level(kStopButton) == 0) || !EmergencyRemote::snapshot().quiesced) {
        EmergencyRemote::cancel_sleep(); gpio_set_level(kScreenEnable, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
        return block == RuntimePolicy::SleepBlock::None ? 4 : static_cast<int>(block);
    }
    sleep_contact_low = contact_low; sleep_cookie = kSleepCookie;
    // ESP-IDF's oneshot unit holds SAR ADC in software power-on mode for the
    // lifetime of the handle. Release it before shutting down the power domains.
    const bool restore_adc = battery_adc != nullptr;
    if (battery_adc) {
        ESP_ERROR_CHECK(adc_oneshot_del_unit(battery_adc));
        battery_adc = nullptr;
    }
    configure_sleep_gpio();
    ESP_LOGI("emergency_ui", "SLEEP_GPIO adc=released gpio0/6/10=isolated hold=off; gpio3=pullup gpio4=floating gpio5=external-pullup");
    gpio_dump_io_configuration(stdout, (1ULL << 0) | (1ULL << 6) | (1ULL << 10));
    ESP_LOGI("emergency_ui", "DEEP_SLEEP contact_low=%u wake=GPIO3/4/5", contact_low);
    // Native USB becomes unavailable here. Restore its PHY through the driver
    // if a wake input arrives during entry and deep sleep is rejected.
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(20));
    isolate_usb_for_sleep();
    err = esp_deep_sleep_try_to_start();
    // A wake signal arriving during entry aborts sleep; restore the screen and service.
    sleep_cookie = 0;
    release_sleep_holds();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_uninstall());
    init_usb_driver();
    if (restore_adc) battery_adc = init_battery_adc();
    ESP_ERROR_CHECK(gpio_set_direction(kScreenEnable, GPIO_MODE_INPUT_OUTPUT));
    EmergencyRemote::cancel_sleep(); gpio_set_level(kScreenEnable, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
    ESP_LOGW("emergency_ui", "SLEEP_ENTRY_ABORTED %s", esp_err_to_name(err));
    return 4;
}
}

namespace EmergencyUi {
void run() {
    constexpr const char* tag = "emergency_ui";
    // Formal runtime: cold starts synchronize OFF; only a saved contact release may request ON.
    init_board_gpio();
    vTaskDelay(pdMS_TO_TICKS(100)); // OLED power and SH1106 reset settling.
    adc_oneshot_unit_handle_t battery_adc = init_battery_adc();
    adc_cali_handle_t battery_calibration = nullptr;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali{};
    cali.unit_id = ADC_UNIT_1;
    cali.chan = kBatteryAdcChannel;
    cali.atten = ADC_ATTEN_DB_12;
    cali.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (battery_adc && adc_cali_create_scheme_curve_fitting(&cali, &battery_calibration) != ESP_OK) {
        ESP_LOGW(tag, "Battery ADC calibration unavailable; raw readings only");
    }
#endif
    init_usb_driver();
    ESP_LOGI(tag, "BOARD INTEGRATION: SDA=%d SCL=%d offset=%d",
        CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
    const bool release_wake = (esp_sleep_get_wakeup_causes() & (1U << ESP_SLEEP_WAKEUP_GPIO)) != 0 &&
        sleep_cookie == kSleepCookie && sleep_contact_low && gpio_get_level(kStopButton) != 0;
    sleep_cookie = 0;
    ESP_ERROR_CHECK(EmergencyRemote::init(release_wake));
    nvs_handle_t settings;
    if (nvs_open("estop_ui", NVS_READONLY, &settings) == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(settings, "always_on", &value) == ESP_OK) always_on = value != 0;
        nvs_close(settings);
    }
    RuntimePolicy::Button button(gpio_get_level(kUiButton) == 0);
    RuntimePolicy::Menu menu;
    RuntimePolicy::FaultNotice notice;
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && saved_fault_check == ~saved_fault &&
        (saved_fault & 0xffffff00U) == kFaultMagic) {
        const int page = saved_fault & 255;
        if (page == 6 || page == 7 || (page >= 10 && page <= 18)) {
            notice.restore(page, esp_timer_get_time(), RuntimeSettings::get("notice_ms") * 1000LL);
            last_fault_page = page;
            ESP_LOGI(tag, "RESTORED_FAULT_HISTORY page=%d", page);
        }
    }
    bool low_notified = false;
    int64_t low_notice_until = 0;
    int message = 0;
    bool manual_sleep_pending = false;
    int64_t activity_at = esp_timer_get_time(), battery_at = activity_at + 30000000;
    int64_t ui_rendered_at = 0, display_retry_at = 0;
    bool fault_acknowledged = false, failed_before = false, screen_blanked = false;
    int64_t battery_report_at = 0;
    bool was_online = false;
    char command_line[128]{}; size_t command_size = 0;
    bool command_overflow = false;
    init_console();
    log_board_snapshot(battery_adc, battery_calibration);
    Sh1106 display;
    esp_err_t err = display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL,
                                CONFIG_ESTOP_OLED_COLUMN_OFFSET);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "OLED init failed: %s; radio control remains active", esp_err_to_name(err));
        display.shutdown();
    }
    ESP_LOGI(tag, "SH1106 detected at 0x%02x; r=pair s=status f=OFF o=ON m=live t=pixels h=hardware", display.address());
    int diagnostic_page = -1;
    int shown_page = -1;
    auto last_state = EmergencyRemote::State::UNPAIRED;
    int64_t state_since = esp_timer_get_time(), rendered_data = -1;
    unsigned frames = 0, failures = 0;
    uint8_t frame[1024];
    while (true) {
        bool dirty = false;
        const int64_t tick = esp_timer_get_time();
        if (tick >= battery_at) { log_board_snapshot(battery_adc, battery_calibration); battery_at = tick + RuntimeSettings::get("battery_ms") * 1000LL; }
        auto gesture = button.update(gpio_get_level(kUiButton) == 0, tick, RuntimeSettings::get("debounce_ms") * 1000LL, RuntimeSettings::get("long_ms") * 1000LL);
        uint8_t key = 0;
        if (usb_serial_jtag_read_bytes(&key, 1, pdMS_TO_TICKS(20)) == 1) {
            if (command_size || command_overflow || key == 'e' || key == '?') {
                if (key == '\r' || key == '\n') {
                    if (command_overflow) printf("ERROR command too long\n");
                    else if (command_size) {
                        command_line[command_size] = 0; int result = 0;
                        const auto rc = esp_console_run(command_line, &result);
                        if (rc != ESP_OK) printf("ERROR %s\n", esp_err_to_name(rc));
                    }
                    command_size = 0; command_overflow = false;
                } else if (key == 8 || key == 127) { if (command_size) --command_size; }
                else if (key >= 32 && key < 127 && !command_overflow) {
                    if (command_size < sizeof(command_line) - 1) command_line[command_size++] = key;
                    else command_overflow = true;
                }
            }
            else if (key == 'j') gesture = RuntimePolicy::Gesture::Short;
            else if (key == 'k') gesture = RuntimePolicy::Gesture::Long;
            else if (key == 'z') manual_sleep_pending = true;
            else if (key == 'r') EmergencyRemote::start_pairing();
            else if (key == 'c') EmergencyRemote::test_wrong_channel();
            else if (key == 'f') EmergencyRemote::request_stop();
            else if (key == 'o') EmergencyRemote::request_on();
            else if (key == 'm' || key == 'v') diagnostic_page = -1;
            else if (key == 't') diagnostic_page = 12;
            else if (key >= '0' && key <= '9') diagnostic_page = key - '0';
            else if (key == 'h') log_board_snapshot(battery_adc, battery_calibration);
            else if (key == 's') {
                const auto s = EmergencyRemote::snapshot();
                ESP_LOGI(tag, "REMOTE_STATUS state=%u paired=%u online=%u stop=%u output=%u protect=%u voltage_mv=%u current_ua=%ld",
                    static_cast<unsigned>(s.state), s.paired, s.online, s.stop_closed,
                    s.output_on, s.protection_mask, s.data.voltage_mv, static_cast<long>(s.data.current_ua));
                ESP_LOGI(tag, "RUNTIME usb=%d always_on=%u busy=%u pairing=%u view=%u item=%d sleep_block=%u battery_mv=%d",
                    gpio_get_level(kVbusDetect), always_on, s.busy, s.pairing, static_cast<unsigned>(menu.view), menu.selected,
                    static_cast<unsigned>(sleep_block()), battery_mv);
            }
            activity_at = tick; dirty = command_size == 0;
        }
        if (gesture != RuntimePolicy::Gesture::None) { activity_at = tick; dirty = true; diagnostic_page = -1; }
        const auto before_ui = EmergencyRemote::snapshot();
        const int current_fault = ProductUi::fault_page(before_ui);
        if (notice.update(current_fault, tick, RuntimeSettings::get("notice_ms") * 1000LL, before_ui.on_attempt)) {
            last_fault_page = notice.page;
            saved_fault = kFaultMagic | static_cast<uint32_t>(notice.page); saved_fault_check = ~saved_fault;
            fault_acknowledged = false;
            activity_at = tick; dirty = true;
            ESP_LOGI(tag, "FAULT_NOTICE page=%d hold_ms=%lu", notice.page, static_cast<unsigned long>(RuntimeSettings::get("notice_ms")));
        }
        const bool display_wake = RuntimePolicy::consume_display_wake(screen_blanked, before_ui.connection_failed, gesture);
        if (display_wake) {
            menu.home(); fault_acknowledged = true; notice.dismiss(); low_notice_until = 0;
            gesture = RuntimePolicy::Gesture::None;
            ESP_LOGI(tag, "DISPLAY_WAKE_ONLY no menu action");
        }
        if (before_ui.connection_failed && !failed_before) { activity_at = tick; menu.home(); }
        failed_before = before_ui.connection_failed;
        if (before_ui.connection_failed && menu.view == RuntimePolicy::View::Home && gesture == RuntimePolicy::Gesture::Short) {
            EmergencyRemote::retry_connection(); gesture = RuntimePolicy::Gesture::None;
        }
        const bool controlling = before_ui.state == EmergencyRemote::State::STOPPING || before_ui.state == EmergencyRemote::State::STARTING;
        if (controlling) { menu.home(); gesture = RuntimePolicy::Gesture::None; }
        else if (gesture != RuntimePolicy::Gesture::None) {
            if (menu.view == RuntimePolicy::View::Home && !fault_acknowledged && notice.page >= 0 &&
                (current_fault >= 0 || notice.holding(tick)) && gesture == RuntimePolicy::Gesture::Short) {
                gesture = RuntimePolicy::Gesture::None; // First acknowledgement opens data, never ON or a hidden menu.
                ESP_LOGI(tag, "FAULT_ACK show data; output unchanged");
            }
            fault_acknowledged = true; notice.dismiss(); low_notice_until = 0;
        }
        const auto old_view = menu.view;
        const auto action = menu.update(gesture, tick, RuntimeSettings::get("menu_idle_ms") * 1000LL);
        dirty = dirty || old_view != menu.view;
        using RuntimePolicy::Action;
        if (action == Action::AlwaysOn) {
            message = save_mode(!always_on) ? 6 : 7; menu.view = RuntimePolicy::View::Message;
        } else if (action == Action::Stop) EmergencyRemote::request_stop();
        else if (action == Action::Sleep) manual_sleep_pending = true;
        else if (action == Action::Pair || action == Action::Repair) {
            const auto s = EmergencyRemote::snapshot();
            if (!s.paired || sleep_block() == RuntimePolicy::SleepBlock::None ||
                (gpio_get_level(kVbusDetect) && !s.output_on && !s.busy && s.output_time_us > 0 && tick - s.output_time_us < 3000000)) {
                EmergencyRemote::start_pairing(action == Action::Repair); message = 8;
            } else message = 4;
            menu.view = RuntimePolicy::View::Message;
        }
        if (manual_sleep_pending && gpio_get_level(kUiButton) == 0) {
            message = 5; menu.view = RuntimePolicy::View::Message; menu.touched = tick;
        }
        if (manual_sleep_pending && gpio_get_level(kUiButton) != 0) {
            manual_sleep_pending = false; message = enter_sleep(display, battery_adc);
            ESP_LOGI(tag, "MANUAL_SLEEP_DENIED reason=%d", message);
            menu.view = RuntimePolicy::View::Message; menu.touched = tick; dirty = true;
        }
        const auto state = EmergencyRemote::snapshot();
        const int64_t now = esp_timer_get_time();
        if (state.state != last_state) { last_state = state.state; state_since = now; if (!state.connection_failed) activity_at = now; fault_acknowledged = false; }
        const bool low = battery_mv > 0 && battery_mv <= static_cast<int>(RuntimeSettings::get("low_mv")) && !gpio_get_level(kVbusDetect);
        if (!low) low_notified = false;
        if (low && !low_notified && !controlling && current_fault < 0 && !state.connection_failed &&
            menu.view == RuntimePolicy::View::Home) {
            low_notified = true; message = 11; menu.view = RuntimePolicy::View::Message;
            menu.touched = now; low_notice_until = now + RuntimeSettings::get("notice_ms") * 1000LL;
            dirty = true;
        }
        const int64_t idle_limit = RuntimeSettings::get(state.connection_failed ? "fail_idle_ms" : "idle_ms") * 1000LL;
        const bool idle = now - activity_at > idle_limit && !notice.holding(now) && now >= low_notice_until;
        if ((!always_on || state.connection_failed) && idle && sleep_block() == RuntimePolicy::SleepBlock::None) {
            message = enter_sleep(display, battery_adc); activity_at = now; dirty = true;
        }
        if (state.online && !state.connection_failed && battery_percent >= 0 && (now >= battery_report_at || !was_online)) {
            EmergencyRemote::report_battery(static_cast<uint8_t>(battery_percent));
            battery_report_at = now + RuntimeSettings::get("report_ms") * 1000LL;
        }
        was_online = state.online && !state.connection_failed;
        int page = static_cast<int>(state.state);
        if (fault_acknowledged && ProductUi::fault_page(state) >= 0) page = 100;
        if ((state.state == EmergencyRemote::State::OFF && (!state.stop_closed || ((gpio_get_level(kVbusDetect) || always_on) && now - state_since > 1500000))) ||
            (state.state == EmergencyRemote::State::ON && now - state_since > 1500000)) page = 100;
        if (!fault_acknowledged && state.state == EmergencyRemote::State::PROTECTED && state.protection_mask) {
            // Multiple active faults prioritize OCP, OTP, OVP, then UVP; log retains the full mask.
            page = (state.protection_mask & 8) ? 18 : (state.protection_mask & 1) ? 15 :
                   (state.protection_mask & 2) ? 16 : 17;
        }
        if (diagnostic_page >= 0 && !state.stop_closed &&
            state.state != EmergencyRemote::State::STOPPING && state.state != EmergencyRemote::State::STARTING)
            page = 200 + diagnostic_page;
        if (!fault_acknowledged && notice.holding(now) && ProductUi::fault_page(state) < 0 && !controlling && !state.connection_failed)
            page = notice.page;
        if (state.pairing && !controlling) page = 19;
        if (page == 100) page = state.online ? 100 : 101;
        const bool urgent = state.state == EmergencyRemote::State::STOPPING ||
            state.state == EmergencyRemote::State::STARTING || (!fault_acknowledged && (state.protection_mask ||
            state.state == EmergencyRemote::State::SHORT_FAULT || state.state == EmergencyRemote::State::DETECT_ERROR));
        if (state.connection_failed && menu.view == RuntimePolicy::View::Home) page = 400;
        if (!urgent && menu.view != RuntimePolicy::View::Home) page = 300 + static_cast<int>(menu.view);
        dirty = dirty || (page >= 300 && now - ui_rendered_at > 1000000);
        dirty = dirty || page != shown_page || (page >= 100 && page < 200 && rendered_data != state.data_time_us);
        const bool blank = idle && menu.view == RuntimePolicy::View::Home &&
            (state.connection_failed || (!always_on && !gpio_get_level(kVbusDetect)));
        if (blank && !screen_blanked) {
            display.shutdown();
            gpio_set_direction(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SDA), GPIO_MODE_INPUT);
            gpio_set_direction(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SCL), GPIO_MODE_INPUT);
            gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SDA), GPIO_FLOATING);
            gpio_set_pull_mode(static_cast<gpio_num_t>(CONFIG_ESTOP_OLED_SCL), GPIO_FLOATING);
            gpio_set_level(kScreenEnable, 0); screen_blanked = true;
        }
        if (!blank && screen_blanked) {
            gpio_set_level(kScreenEnable, 1); vTaskDelay(pdMS_TO_TICKS(100));
            screen_blanked = false; shown_page = -1;
        }
        if (!screen_blanked && !display.address() && now >= display_retry_at) {
            display.shutdown();
            err = display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
            display_retry_at = now + 3000000;
            if (err != ESP_OK) display.shutdown();
        }
        if (dirty && !screen_blanked && display.address()) {
            const bool output_fresh = state.output_time_us > 0 && now - state.output_time_us < RuntimeSettings::get("fresh_ms") * 1000LL;
            const bool closing_unconfirmed = state.connection_failed && state.output_on && state.busy;
            if (page == 400) {
                int64_t remaining = activity_at + idle_limit - now;
                if (notice.until - now > remaining) remaining = notice.until - now;
                if (low_notice_until - now > remaining) remaining = low_notice_until - now;
                const int64_t left = remaining > 0 ? (remaining + 999999) / 1000000 : 0;
                ProductUi::render_failure(frame, static_cast<unsigned>(left), !gpio_get_level(kVbusDetect) && !state.output_on,
                                          closing_unconfirmed);
            } else if (page >= 300) {
                using RuntimePolicy::View;
                if (menu.view == View::Menu || menu.view == View::Confirm) ProductUi::render_menu(frame, menu, always_on);
                else if (menu.view == View::Message) ProductUi::render_message(frame, message, always_on);
                else ProductUi::render_info(frame, battery_mv, gpio_get_level(kVbusDetect));
            } else if (page >= 200) std::memcpy(frame, kPages[page - 200], sizeof(frame));
            else if (page >= 100) ProductUi::render_home(frame, state);
            else ProductUi::render_state(frame, page, notice.history && page == notice.page);
            if (page < 200 || page >= 300)
                ProductUi::render_rail(frame, state, battery_percent, gpio_get_level(kVbusDetect), output_fresh,
                                       ProductUi::fault_page(state) >= 0 || low, closing_unconfirmed);
            ui_rendered_at = now;
            err = display.write_frame(frame, sizeof(frame));
            if (err == ESP_OK) {
                ++frames; shown_page = page; rendered_data = state.data_time_us;
                ESP_LOGI(tag, "FRAME_OK page=%d frames=%u failures=%u", page, frames, failures);
            } else {
                ++failures; shown_page = -1;
                ESP_LOGE(tag, "FRAME_FAILED page=%d error=%s failures=%u", page, esp_err_to_name(err), failures);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
}
}
