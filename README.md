# Wireless Power Emergency Stop

从 `Wireless_power_switch_button` 复制建立的 ESP32-C3 急停开关工程。
当前阶段：工程与 SH1106 OLED bring-up。固件常驻运行，仅显示模拟页面，不启用无线控制、急停输入、电池检测或深度休眠。
最终产品仍按日常深度休眠设计，后续兼容 Lite 和 Pro V2；这些功能尚未完成联调。

## 仓库与 CI/CD

本工程为私有仓库，仅授权成员可以获取源码和固件。
推送 main、提交 PR 或手动运行会触发 CI，分别编译默认屏幕模式和共享遥控组件检查模式，并验证合并固件布局、上传 Actions artifacts。
推送 `vMAJOR.MINOR` 或 `vMAJOR.MINOR.0` 标签触发发布构建，生成 APP、merged 和 SHA256SUMS，发布到本私有仓库的 GitHub prerelease。
当前发布内容仍是 DEMO bring-up 固件。没有公共 CDN、Launchpad 或 firmware-dist 分发；需登录并具有仓库访问权限才能下载。

## 当前接线

| 核心板 | OLED |
|---|---|
| TX / GPIO21 | SDA |
| RX / GPIO20 | SCK / SCL |
| GND | GND |

COM3 为原生 USB Serial/JTAG。OLED 使用 I2C 100 kHz，自动探测 0x3C/0x3D；列偏移默认 2。
菜单 `Emergency stop bring-up` 可配置引脚和列偏移。
正式原理图的 SDA=GPIO1、CLK=GPIO2、STOP_BUTTON=GPIO5 仅记录为后续移植依据，当前未启用。

## 编译与烧录

标准 ESP-IDF v6.0 环境：

```powershell
idf.py build
idf.py -p COM3 flash
```

本机已有安装可用封装入口（路径见脚本；其他电脑请用标准 IDF 环境）：

```powershell
python scripts/idf_local.py build
python scripts/idf_local.py -p COM3 flash
python scripts/verify_display.py --port COM3
```

烧录 bootloader、分区表、应用，不主动擦除 NVS。生成的 merged 镜像属于 bring-up 固件。

## 显示验证

上电默认常驻电压、电流模拟主页。共 10 个中文状态页、在线/离线仪表页和一个 128×64 边框/对角线测试页；发送 `a` 可开启每 4 秒切页的轮播。
页面采用大图标配合短文字：关闭为左侧圆点的空心开关（O），开启为右侧圆点的实心开关（I）；空心停止标志和等待点表示正在关闭、沙漏表示正在开启、放大镜表示检测、警告三角表示短路、盾牌表示保护、断开信号表示通信中断。
USB 串口 115200，发送单字符即可，无需回车：

| 命令 | 行为 |
|---|---|
| 0–9 | 显示对应模拟状态页并暂停轮播 |
| m | 常驻模拟仪表主页：12.34 V、2.400 A、29.62 W |
| l | 显示仪表数据离线页，数值使用横线占位 |
| t | 显示边框测试图并暂停 |
| n | 下一页并暂停 |
| a | 开启轮播 |
| p | 暂停轮播 |

页面：0 就绪、1 已关闭、2 已开启、3 正在关闭、4 正在开启、5 短路检测、6 短路拒绝、7 过流保护、8 关闭未确认/通信中断、9 未配对。
所有业务页面带 DEMO 标识，不表示真实输出状态。
仪表页右上角显示模拟连接和输出标志，下方显示功率和 USB 场景；连接图案当前不代表实测 RSSI。`verify_display.py` 检查全部 13 页后回到仪表主页。
`FRAME_OK` 表示整帧各页的 I2C 传输成功，不等同于物理屏幕的视觉验收。
实物还应确认：中文清晰、四条边完整、无左右错位、方向正确、切页无异常闪烁。

位图由 `scripts/generate_bringup_pages.py` 生成，使用 Pillow 与 Windows 宋体生成本次验证用静态页面；生成头文件已包含，正常编译无需 Python 图像库。
预览位于 `docs/bringup-preview.png`。正式 UI 后续应接入状态模型和动态绘制。

## 结构和后续范围

```text
main/                         高层启动入口与未编译的按钮启动流程参考
components/
  app/                        急停产品应用组件
    app_runtime/              启动上下文、诊断和休眠收尾工具
    battery_voltage/          电池电压采样与校准
    button_input/             按键事务与操作反馈
    power_manager/            唤醒来源和深度休眠管理
    screen_bringup/           模拟页面、USB 命令和传输诊断
    shell_command/            维护命令注册
    status_led/               状态反馈
  bsp/                        板级专用驱动（sh1106、Temperature）
```

- `components/bsp/sh1106`：独立 SH1106 页式帧传输驱动。
- `components/app/screen_bringup`：模拟页面、USB 命令和传输诊断。
- `main/app_main.cpp`：当前仅进入屏幕验证。
- `main/app_main_button_reference.cpp`：保留原按钮启动流程供移植参考，不参与构建。
- 配对、协议、电量估算、日志和遥控链路等共用组件已统一迁移到
  [wireless-power-components](https://github.com/qingmeijiupiao/wireless-power-components)
  公共仓库，由 `main/idf_component.yml` 固定引用，原本地副本已移除；按钮工程迁移前的说明见
  `docs/button-project-reference.md`。
- SH1106 仅供本工程使用，按要求保留本地。Lite 与 Pro V2 未修改。

后续：急停输入与关闭优先事务、具体保护原因协议、Lite/Pro V2 联调，以及硬件改版后的上电和休眠策略。

## 已确认的主页需求（尚未接入真实模式与数据）

- 插电运行，或用户通过按钮选择常亮模式时，常驻电压/电流主页，显示功率计测量数据。
- 日常低功耗行为仍按深度休眠设计；当前 bring-up 固件持续运行用于验屏。
- 急停操作、保护和通信异常的提示优先覆盖普通仪表页；具体返回时机待状态机实现。
- 后续接入 Lite/Pro V2 的 `DeviceData`，电压按 mV、电流按 uA 转换，功率由电压乘电流计算。
- 对端无数据或数据超时时显示离线/横线，避免把旧数据当成实时数据。正式超时阈值、更新频率、按钮手势和设置持久化尚未实现。

驱动命令参考：[SH1106 V2.6 数据手册](https://www.displayfuture.com/Display/datasheet/controller/SH1106.pdf)。

## 共用组件验证

`battery_level`、`blackbox_service`、`espnow_remote`、`espnow_service_remote`、`espnow_link` 等遥控相关组件，以及 `HXC_NVS`、`PWM`、`circular_flash_buffer`、`Interp`、`blackbox`、`ADC`、`wifi_manager`、`shell`、`diagnostic_log` 等通用组件来自固定 Git 版本的公共仓库，位置见 `main/idf_component.yml`。原本地副本已移除，`main/app_main_button_reference.cpp` 只是未编译的启动流程参考。

```powershell
python scripts/idf_local.py -D ESTOP_VALIDATE_SHARED_REMOTE=ON build
python scripts/idf_local.py -D ESTOP_VALIDATE_SHARED_REMOTE=OFF build
```

第一条检查遥控依赖编译，仍使用屏幕入口；第二条恢复默认屏幕配置。未进行急停开关或真实功率计联调。
遥控相关组件固定到 `15525b7a2d3cc0694bbc0b65dcac8335cd73d454`，其余通用组件固定到 `79d506e686ec743ad961ab76c732af96313db54a`，通过 Component Manager 自动下载，不依赖本地相邻目录。
