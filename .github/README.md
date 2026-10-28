# ai-passport-all-in-one

AI Passport 胸牌（ESP32-C3、8 MB Flash、无 PSRAM、240×320 屏、三个按键）的自制固件，C++20 + ESP-IDF 6.1 + LVGL 9.5。
这里是当前版本 **0.5.12** 实际编进固件的全部源码。

主页是时钟和天气，菜单五页、共 41 项功能：

| 菜单页 | 功能 |
| --- | --- |
| 小智 · 语音 | 小智聊天、语音笔记、语音输入、中英翻译、语音记账、语音计算器、待办提醒、闹钟、番茄钟 |
| 电脑 · 时间 | AI 额度、电脑监控、离座锁屏、PPT 翻页、状态牌、二维码、秒表、倒数日、万年历 |
| 天气 · 财经 · 资讯 | 天气预报、台风路径、地震速报、财经新闻、自选股、汇率、百度热榜、Hacker News、历史上的今天 |
| 学习 · 影音 | 背单词、每日一句、知识问答、收音机、播客、白噪音、声音（调音器 / 节拍器） |
| 工具 · 设置 | 手电筒、警示灯、随机转盘、移动监控、WiFi 工具箱、蓝牙声纳、系统设置 |

另外还有「你好小智」语音唤醒，可以说功能名直接跳转。

## 目录

```
firmware/passport_device/       设备工程：CMake、分区表、sdkconfig.defaults，板级驱动、联网与原生服务（main/）
  components/espressif__esp-sr  裁剪后的 esp-sr 2.4.7（C3 库 + 你好小智 唤醒词模型）
  components/espressif__dl_fft  esp-dl 的 dl_fft 0.8.0
firmware/passport_suite/main/   各功能页的界面与逻辑、设置、生成好的中文字体
firmware/passport_suite/components/suite_lvgl/  LVGL 配置和西文字体
firmware/passport_connectivity/main/            主循环、WiFi / 蓝牙 / USB 联网、语音管线
scripts/build_passport_suite.sh 编译脚本（参数 device）
scripts/fetch_deps.sh           下载固定版本的依赖到 deps/
scripts/generate_passport_font.py  按用到的汉字重新生成字体（可选，需要 Pillow）
```

## 编译

需要 [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/)，目标芯片是 `esp32c3`。

```bash
. ~/esp/esp-idf/export.sh                 # 换成你自己的 ESP-IDF 6.1 路径
bash scripts/build_passport_suite.sh device
```

第一次编译时，脚本会运行 `scripts/fetch_deps.sh`，从 Espressif 组件库下载下面这些依赖到 `deps/`，版本固定，并校验 SHA-256：

| 组件 | 版本 |
| --- | --- |
| lvgl/lvgl | 9.5.0 |
| espressif/cjson | 1.7.19~2 |
| espressif/esp_audio_codec | 2.5.0 |
| espressif/esp_codec_dev | 1.6.2 |
| espressif/esp-dsp | 1.8.0 |

编译产物在 `firmware/passport_device/build/`：
- `bootloader/bootloader.bin`
- `partition_table/partition-table.bin`
- `passport_device.bin`
- `srmodels.bin`，唤醒词模型

字体已经生成好放在仓库里。只有修改了界面上的汉字，才需要 `pip install pillow` 重新生成。编译脚本检测到 Pillow 就会自动重新生成。

## 刷机

> ⚠️ 风险自负。刷机前**务必先备份整片 Flash**，这是恢复原厂固件的唯一办法：
>
> ```bash
> python -m esptool --chip esp32c3 read-flash 0 0x800000 backup-8mb.bin
> ```

分区表 `firmware/passport_device/partitions.csv` 保留了原厂固件（rc.48）的数据区，偏移都不变：
- `legacy_nvs`：原厂设置。如果里面有小智的连接信息，会直接沿用；没有的话，固件会走小智 OTA 激活，并在屏幕上显示激活码。
- `cardid`、`storage`、`phy_init`

固件自己的设置放在新增的 `nvs` 分区（0x620000）。

第一次刷入（把 `PORT` 换成你的串口）：

```bash
cd firmware/passport_device/build
python -m esptool --chip esp32c3 -p PORT -b 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 passport_device.bin \
  0x670000 srmodels.bin
python -m esptool --chip esp32c3 -p PORT erase-region 0x620000 0x10000   # 清空新的 nvs 分区
```

以后升级只需要写 `0x10000 passport_device.bin`，还有 `0x670000 srmodels.bin`，这一项只有唤醒词模型变了才需要写。

开机后：
- 按屏幕提示用手机连上胸牌的配网热点，在网页里选择 WiFi；
- 和电脑配合的 API token 由胸牌第一次开机时随机生成。

## 需要电脑助手的功能

以下功能需要 Mac 上的配套助手程序，这个程序不在本仓库：
- 语音输入
- AI 额度、电脑监控、离座锁屏
- 打开 Mac 程序
- USB 联网（PPP）

其余功能只要胸牌连上 WiFi 就能用。

## 许可证

本仓库代码采用 [MIT](../LICENSE)。以下第三方部分保留各自的许可证：
- esp-sr：ESPRESSIF MIT
- dl_fft：MIT
- 中文字体：由思源黑体（SIL OFL 1.1）和 DejaVu Sans 生成
- 状态图标：来自 Font Awesome 5
- `deps/` 里的各个组件
