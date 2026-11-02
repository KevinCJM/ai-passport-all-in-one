# ai-passport-all-in-one

AI Passport 胸牌（ESP32-C3、8 MB Flash、无 PSRAM、240×320 屏、三个按键）的自制固件，C++20 + ESP-IDF 6.1 + LVGL 9.5。
这里是 **0.5.28** 的源码；[最近发布的固件为 0.5.28](https://github.com/KevinCJM/ai-passport-all-in-one/releases/tag/v0.5.28)。

主页是时钟和天气，默认菜单五页、共 43 项功能：

| 菜单页 | 功能 |
| --- | --- |
| 小智 · 语音 | 小智聊天、语音笔记、语音输入、中英翻译、语音记账、语音计算器、待办提醒、闹钟、番茄钟 |
| 电脑 · 时间 | AI 额度、电脑监控、离座锁屏、PPT 翻页、状态牌、二维码、秒表、倒数日、万年历 |
| 天气 · 财经 · 资讯 | 天气预报、台风路径、地震速报、自选股、宏观雷达、汇率、财经新闻、百度热榜、Hacker News |
| 学习 · 影音 | 背单词、每日一句、知识问答、收音机、播客、白噪音、声音（调音器 / 节拍器）、历史上的今天 |
| 工具 · 设置 | 手电筒、警示灯、随机转盘、移动监控、WiFi探测、蓝牙声纳、记分牌、对讲机、系统设置 |

支持「你好小智」语音唤醒和语音控制：说功能名就能跳转，也能用语音记笔记、记账、计算、添加待办和闹钟，或让 Mac 打开程序。唤醒开关在「系统设置 › 唤醒与对讲」；小智语音服务需要联网并完成激活。

## 这次更新

- **菜单自己定**：从43项功能中选择显示哪些，使用原有分类、自定义分页，或不分页的九宫格、紧凑列表，调整页名和顺序；手机网页和 Mac 小助手均可编辑。
- **海外城市**：新增155个海外常用城市，支持语音、手机选择及 IP 定位，天气日期和小时跟随查询城市当地时间；城市输入会显示实际收音、识别或失败状态，超时后可重试。
- **小智绑定提示**：小智聊天和语音识别（笔记、输入、翻译、记账等）是两台小智设备。哪一台还没绑定，用到它的页面就会整屏显示验证码和绑定方法；识别用的那台请在 xiaozhi.me 新建一个智能体再添加，免得识别内容进入聊天记忆。
- **Mac 小助手改版**：升级到 `0.1.8-preview`，窗口和设置网页重新设计。窗口里七个按钮直接打开网页对应部分：菜单自定义、状态牌、倒数日、二维码、打开 Mac 程序、模拟遥控、诊断。状态牌、倒数日和二维码会先读出胸牌上已有的内容，可修改或删除后再保存。
- **联系开发者**：「系统设置 › 联系开发者」可以看到开发者的微信和邮箱。

以上更新包含在 **0.5.28** 中，可在下方「下载固件」获取。

## 开始使用

1. 在主页短按确认打开菜单，上下选择、确认进入；长按上下翻页，长按确认返回。功能页底部会显示当前按键提示。
2. 需要联网时，进入「系统设置 › WiFi 配置 › 添加 WiFi」，用手机连上屏幕显示的胸牌热点，打开 `http://192.168.8.1`，选择家中或手机热点的 2.4 GHz WiFi、输入密码并点「连接 WiFi」。网页显示「连接成功」后手机切回原网络；失败可重新选择网络和填写密码，换网也从同一入口操作。
3. 第一次使用小智时按屏幕提示激活：登录 xiaozhi.me，在智能体里添加设备并输入屏幕上的验证码。语音识别类功能第一次使用时会再显示一个「识别小智」验证码，建议绑到另一个新建的智能体。之后可按键开始对话，或开启语音唤醒。天气、行情、电台等可通过 WiFi 独立使用；语音输入、电脑监控等电脑功能需另开下文的电脑助手（Mac / Windows）。

## 海外城市

「系统设置 › 所在城市」可语音设置，例如「美国纽约」「加拿大温哥华」「日本东京」「韩国首尔」「澳洲悉尼」「法国巴黎」，也可选择「按 IP 定位」。内置中国城市及 155 个海外常用城市，覆盖美国、加拿大、日本、韩国、澳大利亚与欧洲；支持常见别名和英文名。同名城市可加国家区分，例如「英国伦敦」「加拿大伦敦」。

手机配置网页「收听」支持按中文、英文或国家搜索并保存，搜索无需手机联网。IP 定位还可保存城市库以外的城市，取决于网络出口，代理或 VPN 可能影响准确性。天气预报的日期、小时、日出和日落使用查询城市当地时间；主时钟和闹钟沿用原有时区，默认北京时间。

空气质量仍统一按中国国标换算；气象预警仅覆盖中国内地。海外城市的收音机提供现有全国和网络电台，不包含海外本地台。城市数据来源与验证见 [海外城市支持](../docs/international-cities.md)。

## 菜单自定义

在手机配置网页的「菜单」，或 Mac 小助手窗口的「菜单自定义」按钮选择要显示的功能，再调整排列并保存到胸牌。手机入口为「系统设置 › WiFi 配置 › 添加 WiFi」，连上屏幕显示的胸牌热点，打开 `http://192.168.8.1`；修改菜单无需连接外部 WiFi。Mac 小助手需 `0.1.4-preview` 或更新版本，目前只有 macOS 版；紧凑列表需胸牌固件 0.5.26 或更新。

- **原有分类**：保留默认分类，支持隐藏功能、分类排序及分类内排序。
- **自定义分页**：最多8页，每页最多9项；支持页面命名、页面排序、功能跨页移动和页内排序。
- **不分页 · 九宫格**：不分类，所选功能按顺序每九个一屏。
- **不分页 · 紧凑列表**：小图标加名称，每屏七行；和九宫格共用同一个顺序。

拖动或使用上移／下移按钮调整顺序，手机可通过下拉框移动到另一页；预览后点击「保存到胸牌」。各模式保留各自的排列（两种不分页模式共用一份），重启后保留配置。「恢复默认」只恢复菜单布局，仍需点击保存。

胸牌短按上下选择、确认进入；长按上下在分类、分页和九宫格模式翻页，在紧凑列表快速移动七项。长按确认沿用原有返回操作。系统设置始终保留；隐藏只影响菜单入口，仍可语音打开，闹钟、待办提醒和已开启的对讲监听继续生效。

配置读取失败时不能保存，配置冲突时重新读取；只有收到设备写入并应用的确认才显示保存成功。实现与测试见 [菜单自定义](../docs/menu-customization.md)。

## 记分牌

比赛计分用，离线可用。上键给红方加 1 分，下键给蓝方加 1 分，确认撤销上一分。长按上键换赛制（自由计分、乒乓球、羽毛球、排球），长按下键两次清零重开。会自动判断局点、赛点、每局胜负和发球方；每得一分都会保存，休眠或断电也不会丢。详见 [docs/scoreboard.md](../docs/scoreboard.md)。

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

> **平台说明：** 编译和刷机目前只在 macOS 上测试过。电脑助手源码已支持 Windows 10/11（x64，预览），但 Windows 版还没有在真机上验收，详见下文「Windows 版」。胸牌的独立 WiFi 功能不受影响；PPT 翻页通过胸牌自身的蓝牙键盘连接，不需要助手。

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
scripts/passport_menu.js        手机网页与电脑助手共用的离线菜单编辑器
scripts/passport_*.py           电脑助手（入口 passport_assistant.py；Windows 专用部分在 passport_windows.py）
scripts/*.cpp                   助手的原生小工具：Mac 语音输入粘贴、USB PPP 网关（Mac / Windows 共用）
scripts/build_passport_assistant.py  把 Mac 助手打包成 .app 和 .pkg
scripts/build_passport_assistant_windows.py  把 Windows 助手打包成 zip（配合 build_windows_helpers.sh，在 CI 上跑）
tests/                          电脑助手与固件数据解析的宿主测试
```

## 下载固件

- [0.5.28 完整固件包](https://github.com/KevinCJM/ai-passport-all-in-one/releases/download/v0.5.28/ai-passport-0.5.28-esp32c3.zip)：应用、引导程序、分区表、唤醒词模型及刷机说明。
- [仅应用 passport_device.bin](https://github.com/KevinCJM/ai-passport-all-in-one/releases/download/v0.5.28/passport_device.bin)：已安装本项目固件时使用。
- [SHA256SUMS](https://github.com/KevinCJM/ai-passport-all-in-one/releases/download/v0.5.28/SHA256SUMS)：下载校验。

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
- `legacy_nvs`：原厂设置。如果里面有小智的连接信息，会直接沿用；没有的话，固件会走小智 OTA 激活，并在屏幕上显示激活码。语音识别用的第二台小智（MAC 首字节置本地位）总是走 OTA，未绑定时同样显示激活码。
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

`0x670000 srmodels.bin` 只有唤醒词模型变了才需要写。0.5.28 沿用原模型。

开机后：
- 按屏幕提示用手机连上胸牌的配网热点，在网页里选择 WiFi；
- 和电脑配合的 API token 由胸牌第一次开机时随机生成。

## 电脑助手（Mac / Windows）

电脑助手是运行在电脑上的配套程序（源码 `scripts/passport_assistant.py`，当前版本 `0.1.9-preview`）。语音输入、AI 额度、电脑监控、离座锁屏、打开电脑程序和 USB 借网等功能，需要电脑上开着助手。胸牌和电脑之间优先走 USB 线，拔线后自动改走蓝牙。

| | Mac | Windows（预览） |
| --- | --- | --- |
| 系统 | macOS 15 以上，Apple Silicon | Windows 10/11，x64 |
| 界面 | 菜单栏图标 + 助手窗口 + 设置网页 | 右下角托盘图标 + 设置网页 |
| 数据目录 | `~/Library/Application Support/AI Passport/` | `%APPDATA%\AI Passport\` |
| 状态 | 已在真机上日常使用 | CI 在 Windows 桌面上自动测过（插胸牌的部分除外） |

设置网页地址都是 http://127.0.0.1:8768/ ，只有本机能打开。助手一启动网页就能用，胸牌没连上时，读写胸牌的部分会提示「胸牌未连接」。

### 1. 安装

安装包在 [Releases › 电脑助手 0.1.9 预览版](https://github.com/KevinCJM/ai-passport-all-in-one/releases/tag/assistant-v0.1.9-preview)：Mac 下载 `…-macos-arm64.pkg`，Windows 下载 `…-windows-x64.zip`，可用同页的 `SHA256SUMS` 校验。

**Mac**：双击 `.pkg` 安装，助手会装进「应用程序」。安装包没有签名和 Apple 公证，双击安装包或第一次打开时如果提示无法验证，到「系统设置 › 隐私与安全性」点「仍要打开」。

也可以从源码运行或自己打包：

```bash
xcode-select --install                 # 首次需要 Xcode Command Line Tools
brew install opus libslirp
python3.12 -m venv .venv && .venv/bin/pip install -r requirements.txt

.venv/bin/python scripts/passport_assistant.py          # 直接从源码运行
.venv/bin/python scripts/build_passport_assistant.py    # 或者打包成 .app 和 .pkg，输出到 releases/
```

- 打包时会自动下载 FFmpeg 7.1.2 和 LAME 3.100 的源码并编译，第一次要几分钟。
- 打包时会在 `~/Library/Application Support/AI Passport/signing` 生成一个本机用的签名证书，以后每次更新都用它签名，这样系统授权不会因为更新而失效。
- 自己打包的 `.pkg` 安装方法同上。

**Windows**：

1. 从上面的 Releases 页下载 zip。想用最新源码自己打包：在仓库的 Actions 页选「Windows assistant」点「Run workflow」（或推送 `assistant-v*` 标签），跑完在 Artifacts 里下载 `ai-passport-assistant-windows`。
2. 把 zip 解压到一个固定的位置，例如 `D:\Apps\`。之后不要再移动这个文件夹，开机启动记的是这里的路径。
3. 双击 `AI Passport Assistant\AI Passport Assistant.exe`。程序没有代码签名，SmartScreen 会拦截，点「更多信息 › 仍要运行」。

从源码运行（Windows）：安装 Python 3.12 和 MSYS2；在 MSYS2 UCRT64 终端里执行 `bash scripts/build_windows_helpers.sh`，编译 PPP 网关和 ffmpeg；再在命令提示符里执行 `pip install -r scripts\requirements-assistant-windows.txt`、`set PYTHONUTF8=1`，然后 `python scripts\passport_assistant.py`。

### 2. 首次配对（必须用 USB 线）

1. 用**数据线**把胸牌接到电脑。只能充电的线不行。
2. 打开助手。Mac：窗口的「设备」下拉框；Windows：浏览器自动打开的设置网页，最上面「电脑助手 › 设备」。选中显示为 `XX:XX:XX:XX:XX:XX · USB` 的胸牌，点「配对」。
3. 胸牌屏幕和助手上会显示同一个 6 位配对码。Windows 上托盘也会弹通知。两边一致，就在**胸牌上按确认**。
4. 状态变成「胸牌已连接 · USB」就完成了。配对信息保存在数据目录的 `device.json`。

换电脑或在另一台电脑上使用：把旧电脑数据目录里的 `device.json` 拷过去，在新电脑上点「导入配对文件…」。Mac 和 Windows 的配对文件通用。也可以在新电脑上用 USB 重新配对，旧的配对会失效。

### 3. 日常使用

- **插着 USB 线**：走 USB，最稳定，胸牌还能借电脑上网（见下文「USB 联网」）。
- **拔掉线**：进入任何电脑功能页（语音输入、AI 额度、电脑监控、离座锁屏）时，胸牌会打开蓝牙，助手自动改走蓝牙。第一次走蓝牙时系统会请求配对：Mac 上点「连接」或「配对」；Windows 上点「允许」。
- **状态显示**：Mac 看窗口顶部或菜单栏图标的菜单；Windows 把鼠标停在托盘图标上，或右键看第一行。「胸牌已连接 · USB / 蓝牙」表示一切正常。
- **暂停助手**：窗口、托盘菜单或网页上点「暂停助手」，会断开胸牌、释放串口，比如刷机前要先暂停。再点「恢复助手」继续。
- **退出**：Mac 用菜单栏图标里的「退出」；Windows 用托盘右键里的「退出 AI Passport 助手」。关掉浏览器网页不会退出助手。

**Mac 的系统权限**：语音输入需要「辅助功能」权限，用来把文字粘贴到光标处；拔线后需要「蓝牙」权限。助手发现缺少权限时会弹窗，点按钮打开对应的系统设置页，打开「AI Passport Assistant」的开关即可，助手会自动检测。如果开关已经打开仍然无效，先用「－」移除旧条目，再点「在 Finder 显示助手」，把当前的助手拖进列表。Windows 不需要授权。

### 4. 各个电脑功能怎么用

先在胸牌菜单里打开对应的功能页，各页底部都有按键提示。

| 功能 | 怎么用 |
| --- | --- |
| **语音输入** | 先在电脑上把光标放到要输入的地方，例如聊天框、文档、终端。在胸牌按**确认**开始说话，再按**确认**结束，识别出的文字会粘贴到光标处；长按确认放弃这次录音。说完之后，按**下**发送回车，按**上**删除光标所在的整行。不需要安装输入法，用的是小智的语音识别（需要先在胸牌上绑定「识别小智」）。粘贴大约 1 秒后，助手会把剪贴板恢复成原来的文字。 |
| **AI 额度** | 显示 Codex 和 Claude Code 当前 5 小时和 7 天额度的用量。这两个工具需要你自己在电脑上安装并登录。Windows 上 Codex 要能在命令行直接运行 `codex`；Claude Code 读 `%USERPROFILE%\.claude\.credentials.json`。 |
| **电脑监控** | 显示 CPU、内存、磁盘、网速和电池，打开页面就能看。 |
| **离座锁屏** | 胸牌挂在身上，用蓝牙信号判断你是否在座位上，你走开就锁定电脑。上下键选择设置项，确认键修改。可设置：离开距离（近 / 中 / 远）、离开确认（5、10、20、30 秒）、防锁屏（关 / 保持亮屏 / 保持在线，「保持在线」会定时轻移鼠标，避免聊天软件显示离开）、回来亮屏。长按**上**：以当前信号作为「在座」的基准，第一次用时坐在电脑前做一次；长按**下**：立即锁屏。插着 USB 线时不测信号，只有防锁屏和立即锁屏。**Windows 读不到蓝牙信号强度，所以只能防锁屏和立即锁屏，不会自动锁屏。** 解锁电脑仍需要你自己输入密码或使用指纹。 |
| **打开电脑程序** | 对胸牌说「你好小智」，等它开始听，再说「打开电脑的微信」。Mac 上也可以说「打开 Mac 的微信」。直接说「打开微信」也行，但胸牌自己有同名功能（计算器、日历、天气等）时，会打开胸牌上的功能。助手按程序名找：Mac 找「应用程序」文件夹，Windows 找开始菜单。找不到或总找错的，在设置网页「打开电脑程序」里添加口令，指定说哪个词打开哪个程序；页面下方会列出胸牌最近听到的词，可以直接设为口令。这个功能的开关在胸牌「系统设置 › 唤醒与对讲 › 打开 Mac 程序」。 |
| **语音记账** | 在胸牌上记账，胸牌连上电脑后，账目会自动同步到数据目录的 `记账.csv`。点窗口、托盘菜单或网页上的「记账表格」，会用 Numbers 或 Excel 打开它。 |
| **USB 联网** | 胸牌插着 USB 线、又没有 WiFi 时，小智、天气、电台等会借用电脑的网络。这条通道比较慢，只适合应急使用。 |

### 5. 设置网页

可以在 Mac 窗口里点「网页设置」下的按钮，在 Windows 托盘右键选「打开助手」或「网页设置」，也可以直接在浏览器打开 http://127.0.0.1:8768/ 。页面各部分：

- **电脑助手**：连接状态、配对码、设备和配对、导入配对文件、电脑功能检查（语音输入组件、Codex、Mac 的系统权限）。选项有开机自动启动、允许按 IP 定位城市（会把电脑的公网 IP 发给 ipwho.is；不开也可以在胸牌上说城市名），以及记账表格、暂停助手和使用说明。
- **菜单自定义**：选择胸牌菜单里显示哪些功能，以及分页方式和顺序。保存后胸牌菜单马上更新。
- **状态牌 / 倒数日 / 二维码**：页面会先读出胸牌上已有的内容，修改或删除后点「保存到胸牌」。读取失败时不允许保存，避免用空白内容覆盖胸牌上的数据。
- **打开电脑程序**：口令表，以及胸牌最近听到的词。每条口令可以点「试一下」。
- **模拟遥控**：上、下、确认、长按上、长按下、长按确认，效果和按胸牌上的键一样。
- **诊断**：助手和胸牌的连接详情，每 2 秒刷新。遇到问题时点「复制诊断信息」，发给开发者。

开机自动启动：Mac 写入当前用户的登录项，需要从「应用程序」文件夹里运行的助手；Windows 写入当前用户的注册表启动项，不需要管理员权限。

### 6. 常见问题

| 现象 | 处理 |
| --- | --- |
| 「设备」里没有胸牌 | 换一根数据线，或换一个 USB 口。确认没有其他程序（刷机工具、串口监视器）占用串口。 |
| 一直「等待胸牌连接」 | 插着线时，看胸牌是否开机、是否在主页附近。拔线后，要先进入一个电脑功能页，胸牌才会打开蓝牙。 |
| 拔线后连不上蓝牙 | Mac：检查蓝牙权限；系统弹出配对请求时点「连接」。Windows：在「设置 › 蓝牙和设备」里删除旧的 AI-Passport，再进入一次电脑功能页重新配对。 |
| 语音输入识别了但没打字 | Mac：检查辅助功能权限。Windows：目标窗口如果是以管理员身份运行的，就收不到粘贴，这是 Windows 的限制。胸牌上如果显示验证码，说明「识别小智」还没绑定。 |
| 网页提示端口 8768 被占用 | 可能已经开着一个助手，或者有其他程序占用了这个端口。助手不能同时运行两个：再打开一次时，Windows 会直接打开已运行助手的网页。 |
| 要刷机 | 先「暂停助手」或退出助手，释放串口。 |

日志在数据目录里：`gateway.log`（USB 网关）；Mac 版还有 `assistant.log`。反馈问题时，请附上设置网页「诊断」里复制的信息。

### 7. 隐私与卸载

- 配对信息、设置、账本和日志都只保存在本机的数据目录里，不在仓库中，助手也不会上传。语音只发给小智的语音识别服务；开启 IP 定位时，会把公网 IP 发给 ipwho.is。
- 卸载 Mac 版：先取消「登录后自动启动」并退出助手，再删除「应用程序」里的 AI Passport Assistant 和数据目录。
- 卸载 Windows 版：先取消「开机自动启动」并退出助手，再删除解压出来的文件夹和 `%APPDATA%\AI Passport\`。

设计说明见 [Windows 支持设计](../docs/windows-assistant.md)。

### 开发与测试

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
- 打包后的 Mac / Windows 助手里的 FFmpeg、LAME、libopus、libslirp（及 GLib 等依赖）和各个 Python 包，许可证文件随安装包一起附带
