/**
 * @file sh1106.cpp
 * @brief SH1106 OLED 的 I2C 驱动实现：地址探测、初始化命令与页式整帧写入。
 */
#include "sh1106.h"
#include <cstring>
#include <initializer_list>

// 析构时关闭显示并释放资源。
Sh1106::~Sh1106() {
    shutdown();
}

// 关闭显示（命令 0xae），随后注销 I2C 从机并释放总线，最后清空句柄状态。
void Sh1106::shutdown() {
    if (device_) {
        // 首字节 0x00 为控制字节，表示其后为命令流。
        const uint8_t off[] = {0x00, 0xae};
        i2c_master_transmit(device_, off, sizeof(off), 100);
    }
    if (device_) i2c_master_bus_rm_device(device_);
    if (bus_) i2c_del_master_bus(bus_);
    device_ = nullptr; bus_ = nullptr; address_ = 0;
}

// 初始化 I2C 主总线与屏幕：校验参数、探测从机地址、配置设备，
// 然后下发初始化命令并清空显存。重复初始化返回状态错误。
esp_err_t Sh1106::init(int sda, int scl, uint8_t offset) {
    if (bus_) return ESP_ERR_INVALID_STATE;
    // SDA/SCL 不能相同；列偏移超过 4 像素视为非法。
    if (sda == scl || offset > 4) return ESP_ERR_INVALID_ARG;
    offset_ = offset;
    // 使用 -1 让驱动自动选择可用的 I2C 控制器。
    i2c_master_bus_config_t bus{};
    bus.i2c_port = -1;
    bus.sda_io_num = static_cast<gpio_num_t>(sda);
    bus.scl_io_num = static_cast<gpio_num_t>(scl);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus, &bus_);
    if (err != ESP_OK) return err;
    // 依次探测两种常见地址 0x3c / 0x3d，命中即锁定。
    for (uint8_t addr : {uint8_t(0x3c), uint8_t(0x3d)}) {
        if (i2c_master_probe(bus_, addr, 100) == ESP_OK) { address_ = addr; break; }
    }
    if (!address_) return ESP_ERR_NOT_FOUND;
    // 以 7 位地址、400kHz 速率把屏幕挂到总线上。
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address_;
    config.scl_speed_hz = 400000;
    err = i2c_master_bus_add_device(bus_, &config, &device_);
    if (err != ESP_OK) return err;
    // 首字节 0x00 表明后续均为命令；采用 SH1106 页寻址与内置 DC/DC，
    // 因此不使用 SSD1306 的地址窗口命令。各命令依次为：关显示、时钟分频、
    // 多路复用比、显示偏移、起始行、电荷泵、段/行重映射、对比度、预充电、
    // 扫描方向等初始化序列。
    const uint8_t commands[] = {0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f,
        0xd3, 0x00, 0x40, 0xad, 0x8b, 0xa1, 0xc8, 0xda, 0x12,
        0x81, 0x80, 0xd9, 0x22, 0xdb, 0x35, 0xa4, 0xa6};
    err = i2c_master_transmit(device_, commands, sizeof(commands), 100);
    if (err != ESP_OK) return err;
    // 逐页写入：每页先设置页地址与列地址，再发送 1 字节数据控制字（0x40）
    // 加 128 字节全 0 数据，从而清空该页显存。
    for (uint8_t page = 0; page < 8; ++page) {
        const uint8_t position[] = {0x00, uint8_t(0xb0 | page), 0x00, 0x10};
        err = i2c_master_transmit(device_, position, sizeof(position), 100);
        if (err != ESP_OK) return err;
        // 0x40 之后跟随 128 字节数据，共 129 字节。
        uint8_t blank[133] = {0x40};
        err = i2c_master_transmit(device_, blank, sizeof(blank), 100);
        if (err != ESP_OK) return err;
    }
    // 打开显示（命令 0xaf）。
    const uint8_t on[] = {0x00, 0xaf};
    return i2c_master_transmit(device_, on, sizeof(on), 100);
}

// 按页写入整帧：frame 必须为 1024 字节（8 页 × 128 列），
// 每页先发送带列偏移的页地址命令，再发送 1 字节数据控制字与 128 字节像素数据。
esp_err_t Sh1106::write_frame(const uint8_t* frame, size_t size) {
    if (!device_) return ESP_ERR_INVALID_STATE;
    if (!frame || size != 1024) return ESP_ERR_INVALID_ARG;
    for (uint8_t page = 0; page < 8; ++page) {
        // 低 4 位列地址与高 4 位列地址分两条命令下发，offset_ 补偿 SH1106 的列偏移。
        const uint8_t position[] = {0x00, uint8_t(0xb0 | page),
            uint8_t(offset_ & 0x0f), uint8_t(0x10 | (offset_ >> 4))};
        esp_err_t err = i2c_master_transmit(device_, position, sizeof(position), 100);
        if (err != ESP_OK) return err;
        // 首字节 0x40 表示后续为显存数据，其后跟上该页的 128 列像素。
        uint8_t data[129];
        data[0] = 0x40;
        memcpy(data + 1, frame + page * 128, 128);
        err = i2c_master_transmit(device_, data, sizeof(data), 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
