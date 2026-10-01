# OLED 中文点阵字体

采用 [Fusion Pixel Font / 缝合像素字体](https://github.com/TakWolf/fusion-pixel-font)，固定发行版 `2026.09.25`，简体中文 `zh_hans`、等宽版本。

- 正文、菜单、右侧输出状态：原生 12 像素。
- 底部操作提示、历史原因、倒计时说明：原生 10 像素。
- 生成时直接绘制为 1 bit 点阵，不缩放、不做灰度刷新。数字、单位及图标沿用已经确认的样式。

字体文件仅用于离线生成页面，设备内只编译生成的 128×64 位图，不装载完整字库。现有两个生成脚本内的 173 个汉字在两个尺寸中均有字形，页面文字边界检查通过。

`source.json` 保存发行版、下载地址及原始 OTF 文件 SHA-256。字体采用 SIL OFL 1.1；保留 `LICENSE-OFL`、发行包的 `OFL.txt`、`LICENSES/` 上游声明和 `UPSTREAM-README.md`。作者构建工具的 MIT 许可不替代字体的 OFL 许可。

生成产品字模：`python scripts/generate_product_ui.py`。
生成页面预览：`python scripts/product_ui_layout.py`，输出至 `tmp/ui-previews/`。
生成新旧对比：`python scripts/compare_ui_fonts.py`，输出 `tmp/ui-previews/font-comparison.png`。

这些步骤需要 Windows、Pillow 和现有西文字体；CI 直接构建已经生成的头文件，不联网下载字体。
