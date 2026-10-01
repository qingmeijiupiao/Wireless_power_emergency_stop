#include "sh1106.h"
#include <cstring>
#include <initializer_list>

Sh1106::~Sh1106() {
    shutdown();
}

void Sh1106::shutdown() {
    if (device_) {
        const uint8_t off[] = {0x00, 0xae};
        i2c_master_transmit(device_, off, sizeof(off), 100);
    }
    if (device_) i2c_master_bus_rm_device(device_);
    if (bus_) i2c_del_master_bus(bus_);
    device_ = nullptr; bus_ = nullptr; address_ = 0;
}

esp_err_t Sh1106::init(int sda, int scl, uint8_t offset) {
    if (bus_) return ESP_ERR_INVALID_STATE;
    if (sda == scl || offset > 4) return ESP_ERR_INVALID_ARG;
    offset_ = offset;
    i2c_master_bus_config_t bus{};
    bus.i2c_port = -1;
    bus.sda_io_num = static_cast<gpio_num_t>(sda);
    bus.scl_io_num = static_cast<gpio_num_t>(scl);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus, &bus_);
    if (err != ESP_OK) return err;
    for (uint8_t addr : {uint8_t(0x3c), uint8_t(0x3d)}) {
        if (i2c_master_probe(bus_, addr, 100) == ESP_OK) { address_ = addr; break; }
    }
    if (!address_) return ESP_ERR_NOT_FOUND;
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address_;
    config.scl_speed_hz = 100000;
    err = i2c_master_bus_add_device(bus_, &config, &device_);
    if (err != ESP_OK) return err;
    // SH1106 页寻址与内置 DC/DC；不使用 SSD1306 的地址窗口命令。
    const uint8_t commands[] = {0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f,
        0xd3, 0x00, 0x40, 0xad, 0x8b, 0xa1, 0xc8, 0xda, 0x12,
        0x81, 0x80, 0xd9, 0x22, 0xdb, 0x35, 0xa4, 0xa6};
    err = i2c_master_transmit(device_, commands, sizeof(commands), 100);
    if (err != ESP_OK) return err;
    for (uint8_t page = 0; page < 8; ++page) {
        const uint8_t position[] = {0x00, uint8_t(0xb0 | page), 0x00, 0x10};
        err = i2c_master_transmit(device_, position, sizeof(position), 100);
        if (err != ESP_OK) return err;
        uint8_t blank[133] = {0x40};
        err = i2c_master_transmit(device_, blank, sizeof(blank), 100);
        if (err != ESP_OK) return err;
    }
    const uint8_t on[] = {0x00, 0xaf};
    return i2c_master_transmit(device_, on, sizeof(on), 100);
}

esp_err_t Sh1106::write_frame(const uint8_t* frame, size_t size) {
    if (!device_) return ESP_ERR_INVALID_STATE;
    if (!frame || size != 1024) return ESP_ERR_INVALID_ARG;
    for (uint8_t page = 0; page < 8; ++page) {
        const uint8_t position[] = {0x00, uint8_t(0xb0 | page),
            uint8_t(offset_ & 0x0f), uint8_t(0x10 | (offset_ >> 4))};
        esp_err_t err = i2c_master_transmit(device_, position, sizeof(position), 100);
        if (err != ESP_OK) return err;
        uint8_t data[129];
        data[0] = 0x40;
        memcpy(data + 1, frame + page * 128, 128);
        err = i2c_master_transmit(device_, data, sizeof(data), 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
