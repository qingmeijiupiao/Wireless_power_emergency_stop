#include "screen_bringup.h"
#include "sh1106.h"
#include "pages.h"
#include "sdkconfig.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ScreenBringup {
void run() {
    constexpr const char* tag = "screen_bringup";
    // 此入口仅进行显示验证，不初始化原按钮的 GPIO、电池、无线或休眠流程。
    usb_serial_jtag_driver_config_t usb{};
    usb.rx_buffer_size = 256;
    usb.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    ESP_LOGI(tag, "DEMO ONLY: no radio commands, no sleep; SDA=%d SCL=%d offset=%d",
        CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL, CONFIG_ESTOP_OLED_COLUMN_OFFSET);
    Sh1106 display;
    esp_err_t err = display.init(CONFIG_ESTOP_OLED_SDA, CONFIG_ESTOP_OLED_SCL,
                                CONFIG_ESTOP_OLED_COLUMN_OFFSET);
    if (err != ESP_OK) {
        while (true) {
            ESP_LOGE(tag, "OLED init failed: %s; check power, GND, SDA, SCL then reset", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    ESP_LOGI(tag, "SH1106 detected at 0x%02x; commands: 0-9=status m=meter l=offline t=pixels n=next a=auto p=pause", display.address());
    // bring-up 默认常驻模拟仪表页；正式 USB/常亮模式选择后续由运行策略决定。
    int page = 10;
    bool automatic = false;
    bool dirty = true;
    int64_t next = esp_timer_get_time() + 4000000;
    unsigned frames = 0, failures = 0;
    while (true) {
        uint8_t key = 0;
        if (usb_serial_jtag_read_bytes(&key, 1, pdMS_TO_TICKS(20)) == 1) {
            if (key >= '0' && key <= '9') { page = key - '0'; automatic = false; dirty = true; }
            else if (key == 'm') { page = 10; automatic = false; dirty = true; }
            else if (key == 'l') { page = 11; automatic = false; dirty = true; }
            else if (key == 't') { page = 12; automatic = false; dirty = true; }
            else if (key == 'n') { page = (page + 1) % kPageCount; automatic = false; dirty = true; }
            else if (key == 'a') { automatic = true; next = esp_timer_get_time() + 4000000; }
            else if (key == 'p') { automatic = false; }
            if (key != '\r' && key != '\n') ESP_LOGI(tag, "command=%c page=%d auto=%d", key, page, automatic);
        }
        if (automatic && esp_timer_get_time() >= next) {
            page = (page + 1) % kPageCount; dirty = true;
            next = esp_timer_get_time() + 4000000;
        }
        if (dirty) {
            err = display.write_frame(kPages[page], sizeof(kPages[page]));
            if (err == ESP_OK) {
                ++frames;
                ESP_LOGI(tag, "FRAME_OK page=%d name=%s frames=%u failures=%u heap=%lu", page, kPageNames[page], frames, failures, (unsigned long)esp_get_free_heap_size());
                dirty = false;
            } else {
                ++failures;
                ESP_LOGE(tag, "FRAME_FAILED page=%d error=%s failures=%u", page, esp_err_to_name(err), failures);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }
}
}
