# ai-passport-all-in-one

AI Passport 胸牌（ESP32-C3、8 MB Flash、无 PSRAM、240×320 屏、三个按键）的自制固件，C++20 + ESP-IDF 6.1 + LVGL 9.5。
这里是当前版本 **0.5.13** 实际编进固件的全部源码。

主页是时钟和天气，菜单五页、共 41 项功能：

| 菜单页 | 功能 |
| --- | --- |
| 小智 · 语音 | 小智聊天、语音笔记、语音输入、中英翻译、语音记账、语音计算器、待办提醒、闹钟、番茄钟 |
| 电脑 · 时间 | AI 额度、电脑监控、离座锁屏、PPT 翻页、状态牌、二维码、秒表、倒数日、万年历 |
| 天气 · 财经 · 资讯 | 天气预报、台风路径、地震速报、财经新闻、自选股、汇率、百度热榜、Hacker News、历史上的今天 |
| 学习 · 影音 | 背单词、每日一句、知识问答、收音机、播客、白噪音、声音（调音器 / 节拍器） |
| 工具 · 设置 | 手电筒、警示灯、随机转盘、移动监控、WiFi 工具箱、蓝牙声纳、系统设置 |

另外还有「你好小智」语音唤醒，可以说功能名直接跳转。

> **平台说明：** 目前只在 macOS 上测试过（编译、刷机和 Mac 助手）。Windows 暂不支持电脑端助手，所以 USB 联网、蓝牙连电脑、语音输入、AI 额度这些要配合电脑的功能在 Windows 上都用不了。只靠 WiFi 的功能不受影响。

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
scripts/passport_*.py           Mac 助手（入口 passport_assistant.py）
scripts/*.cpp                   Mac 助手的两个原生小工具：语音输入粘贴、USB PPP 网关
scripts/build_passport_assistant.py  把 Mac 助手打包成 .app 和 .pkg
tests/                          Mac 助手的单元测试
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

## Mac 助手

`scripts/passport_assistant.py` 是 Mac 上的配套程序，运行后在菜单栏显示图标，设置页面在 http://127.0.0.1:8768/ 。

### 哪些功能要用到助手

**必须开着助手才能用：**

| 功能 | 说明 |
| --- | --- |
| 语音输入 | 胸牌录音，交给小智识别，再把文字粘贴到当前光标处（`mac_dictation_bridge.cpp`） |
| AI 额度 | 显示 Codex 和 Claude Code 的用量，需要你自己安装并登录这两个工具 |
| 电脑监控 | CPU、内存、磁盘、网速、电池 |
| 离座锁屏 | 根据蓝牙信号强弱锁屏 |
| 打开 Mac 程序 | 对胸牌说「打开 Mac 的微信」，口令可以在设置页自定义 |
| USB 联网 | 插着 USB 线时，通过 PPP 共享电脑的网络（`passport_ppp_gateway.cpp`） |
| 蓝牙连接 | 上面这些电脑功能在拔掉 USB 线后自动改走蓝牙 |

**胸牌上能用，助手只负责同步或编辑：**

| 功能 | 助手做什么 |
| --- | --- |
| 语音记账 | 把账目同步成 CSV 文件，点助手窗口或菜单栏里的「记账表格」打开 |
| 状态牌、倒数日、二维码 | 在助手的「设备内容」页编辑内容，再发送到胸牌；状态牌和倒数日也可以直接在胸牌上用语音添加 |

**不需要助手：** 其余功能只要胸牌连上 WiFi 就能用。PPT 翻页是胸牌直接当蓝牙键盘连电脑，也不需要助手。

助手只支持 macOS，没有 Windows 版。需要 macOS 15 以上、Apple Silicon、Xcode Command Line Tools，以及 Homebrew 装的 `opus` 和 `libslirp`。

```bash
brew install opus libslirp
python3.12 -m venv .venv && .venv/bin/pip install -r requirements.txt

.venv/bin/python scripts/passport_assistant.py          # 直接从源码运行
.venv/bin/python scripts/build_passport_assistant.py    # 或者打包成 .app 和 .pkg 安装包，输出到 releases/
```

关于打包：
- 第一次打包会自动下载 FFmpeg 7.1.2 和 LAME 3.100 的源码并编译，需要几分钟。
- 打包时会在 `~/Library/Application Support/AI Passport/signing` 里生成一个本机用的签名证书，以后每次更新都用它签名。
- 安装包没有经过 Apple 公证。

**首次配对：** 用 USB 数据线连接胸牌，按助手的提示在胸牌上核对配对码，然后按确认键。配对信息、设置和日志都保存在 `~/Library/Application Support/AI Passport/`，不在仓库里。

测试：`cd tests && ../.venv/bin/python -m unittest`

## 许可证

本仓库代码采用 [MIT](../LICENSE)。以下第三方部分保留各自的许可证：
- esp-sr：ESPRESSIF MIT
- dl_fft：MIT
- 中文字体：由思源黑体（SIL OFL 1.1）和 DejaVu Sans 生成
- 状态图标：来自 Font Awesome 5
- `deps/` 里的各个组件
- 打包后的 Mac 助手里的 FFmpeg、LAME、libopus、libslirp 和各个 Python 包，许可证文件随安装包一起附带
