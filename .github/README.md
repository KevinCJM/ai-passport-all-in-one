# ai-passport-all-in-one

AI Passport 胸牌（ESP32-C3、8 MB Flash、无 PSRAM、240×320 屏、三个按键）的自制固件，C++20 + ESP-IDF 6.1 + LVGL 9.5。
这里是开发版本 **0.5.24** 的源码；[最近发布的固件为 0.5.21](https://github.com/KevinCJM/ai-passport-all-in-one/releases/tag/v0.5.21)。

主页是时钟和天气，默认菜单五页、共 43 项功能：

| 菜单页 | 功能 |
| --- | --- |
| 小智 · 语音 | 小智聊天、语音笔记、语音输入、中英翻译、语音记账、语音计算器、待办提醒、闹钟、番茄钟 |
| 电脑 · 时间 | AI 额度、电脑监控、离座锁屏、PPT 翻页、状态牌、二维码、秒表、倒数日、万年历 |
| 天气 · 财经 · 资讯 | 天气预报、台风路径、地震速报、自选股、宏观雷达、汇率、财经新闻、百度热榜、Hacker News |
| 学习 · 影音 | 背单词、每日一句、知识问答、收音机、播客、白噪音、声音（调音器 / 节拍器）、历史上的今天 |
| 工具 · 设置 | 手电筒、警示灯、随机转盘、移动监控、WiFi探测、蓝牙声纳、对讲机、系统设置 |

支持「你好小智」语音唤醒和语音控制：说功能名就能跳转，也能用语音记笔记、记账、计算、添加待办和闹钟，或让 Mac 打开程序。唤醒开关在「系统设置 › 唤醒与对讲」；小智语音服务需要联网并完成激活。

## 这次更新

- **菜单自己定**：从43项功能中选择显示哪些，使用原有分类、连续菜单或自定义分页，调整页名和顺序；手机网页和 Mac 小助手均可编辑。
- **海外城市**：新增155个海外常用城市，支持语音、手机选择及 IP 定位，天气日期和小时跟随查询城市当地时间；城市输入会显示实际收音、识别或失败状态，超时后可重试。
- **小助手菜单编辑器**：Mac 小助手升级到 `0.1.2-preview`，通过已配对的 USB 或蓝牙连接保存菜单。

上述功能包含在开发版本 **0.5.24** 中；下方公开下载仍为 **0.5.21**，尚不包含本次更新。使用新版请从当前源码编译。

## 开始使用

1. 在主页短按确认打开菜单，上下选择、确认进入；长按上下翻页，长按确认返回。功能页底部会显示当前按键提示。
2. 需要联网时，进入「系统设置 › WiFi 配置 › 添加 WiFi」，用手机连上屏幕显示的胸牌热点，打开 `http://192.168.8.1`，选择家中或手机热点的 2.4 GHz WiFi、输入密码并点「连接 WiFi」。网页显示「连接成功」后手机切回原网络；失败可重新选择网络和填写密码，换网也从同一入口操作。
3. 第一次使用小智时按屏幕提示激活，之后可按键开始对话，或开启语音唤醒。天气、行情、电台等可通过 WiFi 独立使用；语音输入、电脑监控等电脑功能需另开下文的 Mac 小助手。

## 海外城市

「系统设置 › 所在城市」可语音设置，例如「美国纽约」「加拿大温哥华」「日本东京」「韩国首尔」「澳洲悉尼」「法国巴黎」，也可选择「按 IP 定位」。内置中国城市及 155 个海外常用城市，覆盖美国、加拿大、日本、韩国、澳大利亚与欧洲；支持常见别名和英文名。同名城市可加国家区分，例如「英国伦敦」「加拿大伦敦」。

手机配置网页「收听」支持按中文、英文或国家搜索并保存，搜索无需手机联网。IP 定位还可保存城市库以外的城市，取决于网络出口，代理或 VPN 可能影响准确性。天气预报的日期、小时、日出和日落使用查询城市当地时间；主时钟和闹钟沿用原有时区，默认北京时间。

空气质量仍统一按中国国标换算；气象预警仅覆盖中国内地。海外城市的收音机提供现有全国和网络电台，不包含海外本地台。城市数据来源与验证见 [海外城市支持](../docs/international-cities.md)。

## 菜单自定义

在手机配置网页的「菜单」，或 Mac 小助手「设备内容 › 菜单自定义」选择要显示的功能，再调整排列并保存到胸牌。手机入口为「系统设置 › WiFi 配置 › 添加 WiFi」，连上屏幕显示的胸牌热点，打开 `http://192.168.8.1`；修改菜单无需连接外部 WiFi。Mac 小助手需 `0.1.2-preview` 或更新版本，目前只有 macOS 版。

- **原有分类**：保留默认分类，支持隐藏功能、分类排序及分类内排序。
- **连续菜单**：不分类、不分页，所选功能按顺序连续滚动，画面最多显示九个图标。
- **自定义分页**：最多8页，每页最多9项；支持页面命名、页面排序、功能跨页移动和页内排序。

拖动或使用上移／下移按钮调整顺序，手机可通过下拉框移动到另一页；预览后点击「保存到胸牌」。三种模式保留各自的排列，重启后保留配置。「恢复默认」只恢复菜单布局，仍需点击保存。

胸牌短按上下选择、确认进入；长按上下在分页模式翻页，在连续菜单快速移动九项。长按确认沿用原有返回操作。系统设置始终保留；隐藏只影响菜单入口，仍可语音打开，闹钟、待办提醒和已开启的对讲监听继续生效。

配置读取失败时不能保存，配置冲突时重新读取；只有收到设备写入并应用的确认才显示保存成功。实现与测试见 [菜单自定义](../docs/menu-customization.md)。

## 对讲机

连在同一个 WiFi 下的胸牌按频道（1–9）互相对讲，用 UDP 广播传 Opus（16 kHz，16 kbps）。

- 按键：确认开始说话，再按一次结束（最长 30 秒）；上下调音量；长按上下换频道。
- 后台接收：在「设置 › 唤醒与对讲」打开后，胸牌在任何页面收到对讲都会自动跳到对讲机，讲完 5 秒回到原页面。代价是不再深度睡眠，待机时间变短。
- 网络：手机热点、家用路由器都可以。开了客户端隔离的网络（很多公司和访客 WiFi）会挡掉广播，就收不到。
- `scripts/walkie_mac.py` 可以让 Mac 充当一块胸牌来收发，需要 `brew install opus`。

## 宏观雷达

通过 WiFi / USB PPP 直接读取免费公开数据，无需个人 API Key，也不需要在电脑上安装 AKShare。进入后直接显示中国页；顶部始终显示三个标签，用不同颜色标识当前页。

| 页面 | 颜色 | 固定六项指标 |
| --- | --- | --- |
| 中国 | 青绿 | GDP累计同比、CPI同比、PPI同比、制造业PMI、M2同比、一年期LPR |
| 美国 | 蓝色 | 联邦基金有效利率、十年美债收益率、CPI同比、失业率、非农新增就业、实际GDP环比折年率 |
| 商品 | 金色 | 大宗商品价格指数、BDI干散货指数、美国原油产量、黄金ETF持仓、白银ETF持仓、农产品批发价格总指数 |

| 页面 | 上下短按 | 确认短按 | 长按上 | 长按下 | 长按确认 |
| --- | --- | --- | --- | --- | --- |
| 指标列表 | 选择指标 | 查看走势 | 上一页 | 下一页 | 返回功能列表 |
| 指标详情 | 上一 / 下一指标 | 切换近12 / 60期 | 刷新当前指标 | 无操作 | 返回列表 |

不支持语音添加、删除或编辑指标；仍可语音打开宏观雷达。每次只更新当前页，换页会取消旧请求。断网可查看各页上次成功的数据和各页最近缓存的一条走势。日期表示数据所属期，12/60期表示有效观测点数；黄金、白银ETF显示持仓吨数。美国数据来自 FRED，中国与商品使用东方财富、金十公开端点。详细口径与验证记录见 [宏观雷达](../docs/macro-radar.md)。

> **平台说明：** 编译、刷机和小助手目前只在 macOS 上测试过。小助手没有 Windows 版；依赖助手的 USB 联网、语音输入、AI 额度和电脑监控等功能需使用 Mac。胸牌的独立 WiFi 功能不受影响；PPT 翻页通过胸牌自身的蓝牙键盘连接，不需要小助手。

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
data/international-cities.json  海外城市维护源（generate_passport_cities.py 同步生成固件及手机城市表）
scripts/passport_menu.js        手机网页与 Mac 助手共用的离线菜单编辑器
scripts/passport_*.py           Mac 助手（入口 passport_assistant.py）
scripts/*.cpp                   Mac 助手的两个原生小工具：语音输入粘贴、USB PPP 网关
scripts/build_passport_assistant.py  把 Mac 助手打包成 .app 和 .pkg
tests/                          Mac 助手与固件数据解析的宿主测试
```

## 下载固件

- [0.5.21 完整固件包](https://github.com/KevinCJM/ai-passport-all-in-one/releases/download/v0.5.21/ai-passport-0.5.21-esp32c3.zip)：应用、引导程序、分区表、唤醒词模型及刷机说明。
- [仅应用 passport_device.bin](https://github.com/KevinCJM/ai-passport-all-in-one/releases/download/v0.5.21/passport_device.bin)：已安装本项目固件时使用。
- [SHA256SUMS](https://github.com/KevinCJM/ai-passport-all-in-one/releases/download/v0.5.21/SHA256SUMS)：下载校验。

适用于上述 ESP32-C3、8 MB Flash、无PSRAM的 AI Passport。升级保留原分区表和设置，只写应用；首次安装按下文「刷机」操作。下载包解压后的文件布局与编译产物一致。

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

以后升级只需要写应用，保留分区表、设置和身份数据：

```bash
python -m esptool --chip esp32c3 -p PORT -b 921600 write-flash \
  --flash-mode keep --flash-freq keep --flash-size keep 0x10000 passport_device.bin
```

`0x670000 srmodels.bin` 只有唤醒词模型变了才需要写。0.5.24 沿用原模型。

开机后：
- 按屏幕提示用手机连上胸牌的配网热点，在网页里选择 WiFi；
- 和电脑配合的 API token 由胸牌第一次开机时随机生成。

## Mac 助手

`scripts/passport_assistant.py` 是 Mac 上的配套程序，当前源码版本为 `0.1.2-preview`。运行后在菜单栏显示图标，设置页面在 http://127.0.0.1:8768/ 。

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
| 菜单自定义 | 在「设备内容 › 菜单自定义」编辑显示项、分页和顺序；也可用胸牌的手机配置网页操作，不需要常开助手 |

**不需要助手：** 天气、财经、电台等通过胸牌自己的 WiFi 使用；时钟、计时、手电筒等本地工具无需网络。PPT 翻页是胸牌直接当蓝牙键盘连电脑，也不需要助手。

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

宏观数据解析与缓存回归：`python3 -m unittest discover -s tests -p 'test_passport_macro*.py' -v`，需要本机 C++ 编译器；测试使用 ASan/UBSan。

菜单与城市回归：`python3 tests/test_passport_menu.py -v`、`python3 tests/test_passport_cities.py -v`，需 C++20 编译器及 Node.js。真实 C3 固件的模拟器、HTTP 保存和隔离浏览器测试命令分别见 [菜单自定义](../docs/menu-customization.md) 与 [海外城市支持](../docs/international-cities.md)。模拟器验证不代替实机的语音、射频和按键体验验收。

## 许可证

本仓库代码采用 [MIT](../LICENSE)。以下第三方部分保留各自的许可证：
- esp-sr：ESPRESSIF MIT
- dl_fft：MIT
- 中文字体：由思源黑体（SIL OFL 1.1）和 DejaVu Sans 生成
- 状态图标：来自 Font Awesome 5
- 城市坐标：GeoNames（CC BY 4.0），来源见 [海外城市支持](../docs/international-cities.md)
- `deps/` 里的各个组件
- 打包后的 Mac 助手里的 FFmpeg、LAME、libopus、libslirp 和各个 Python 包，许可证文件随安装包一起附带
