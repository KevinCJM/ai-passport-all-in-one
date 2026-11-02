# 电脑助手：Windows 支持设计

目标：同一份 Python 源码，同时支持 macOS 和 Windows 10/11 x64，胸牌固件不改。

## 1. 现状：Mac 绑定点

| 模块 | Mac 实现 | 问题 |
|---|---|---|
| 窗口 / 菜单栏 | AppKit（`gui()`） | Windows 没有 |
| 单实例 | `fcntl.flock` | Windows 没有 fcntl |
| 开机启动 | LaunchAgent + `launchctl` | — |
| 数据目录 | `~/Library/Application Support/AI Passport` | — |
| USB PPP 网关 | C++ + libslirp，`termios` 直接打开 `/dev/cu.*` | Windows 串口不能 `poll()` |
| 语音输入粘贴 | NSPasteboard + CGEvent 辅助程序（需辅助功能权限） | — |
| 删除整行 / 回车 | CGEvent 键码 | — |
| 离座锁屏 | SACLockScreen、caffeinate、CGEvent 鼠标微动 | — |
| 打开电脑程序 | 扫描 `.app` + `open` | — |
| AI 额度 · Claude | 钥匙串 `security` | Windows 上凭据在 `~/.claude/.credentials.json` |
| 蓝牙 | bleak CoreBluetooth，读加密特征时系统自动配对 | WinRT 不会自动配对；连接后读不到 RSSI |
| Opus / ffmpeg | `.dylib` / 自编译 ffmpeg | 换成 Windows 二进制 |
| 打包 | PyInstaller `.app` + `.pkg` + 本地签名 | 换成 PyInstaller 目录 + zip |

## 2. 总体结构

```
                 ┌──────────────── 本机网页 127.0.0.1:8768（两平台共用的界面）────────────────┐
 托盘图标(Win) ─┤  助手：状态 / 配对码 / 设备 / 选项     设置：菜单、状态牌…、模拟遥控、诊断      │
 菜单栏(Mac)  ─┘                                                                                │
                Controller（配对、重连、依赖检查）── Device（USB 优先，蓝牙备用）── Services   │
                                    │                                                          │
         passport_windows.py（Win32 ctypes：剪贴板、SendInput、锁屏、空闲、防睡眠、开机启动） │
```

原则：

1. **网页就是界面。** 助手的网页服务一启动就开着（以前要等胸牌连上），新加「助手」一节：状态、配对码、设备列表、配对、暂停、开机启动、IP 定位、记账表格。Windows 只需要一个托盘图标（pystray），点开就是这个网页；Mac 继续保留原生窗口，网页也能用。
2. **平台差异只放在边上。** Win32 调用集中在 `passport_windows.py`；其余模块只在用到的地方 `if sys.platform=='win32'` 换实现，业务逻辑不分叉。
3. **固件、通信协议、配对流程都不改。** USB PPP、蓝牙 GATT、令牌鉴权保持一致，所以 Mac 和 Windows 可以配对同一块胸牌，配对文件 `device.json` 也能互相导入。

## 3. 各部分设计

### 3.1 USB：PPP 网关

网关本来就有 TCP 模式（模拟器用的）：监听 `127.0.0.1:<port>`，收到一个连接后，把它当作 PPP 线路来跑。

- **Windows**：`passport-ppp.exe --port <空闲端口>`，由 Python 用 pyserial 打开 `COMx`，再用两个线程在串口和 TCP 之间双向转发（`passport_computer_link.SerialBridge`）。Windows 的 COM 口本身就是独占打开，相当于 Mac 上的 `TIOCEXCL`。
- **Mac**：保持原样，网关直接打开 `/dev/cu.*`。桥接模式在 Mac 上也能跑，可以用真机测试 Windows 的数据通路。
- **C++ 改动**：`poll`→`WSAPoll`，`read/write`→`recv/send`，需要 `WSAStartup`，信号处理换成 `SetConsoleCtrlHandler`；`--serial` 在 Windows 上返回 2。libslirp 和 glib 从 MSYS2 UCRT64 获取，相关 DLL 和 exe 放在同一个目录。
- 端口识别不变：VID `303A`、PID `1001`，序列号就是 MAC。Windows 的 pyserial 会从复合设备的父节点读出序列号；读到的格式可能没有冒号，所以先统一格式再比较。

### 3.2 语音输入（粘贴与快捷键）

| 动作 | Mac | Windows |
|---|---|---|
| 粘贴 | 剪贴板 + ⌘V | `CF_UNICODETEXT` 写入剪贴板 + `SendInput` Ctrl+V；同时标记「不进剪贴板历史 / 云同步」 |
| 恢复剪贴板 | 恢复所有类型 | 只恢复文本（一般够用；图片等其他格式不恢复） |
| 回车 | 键码 36 | `VK_RETURN` |
| 删除整行 | 终端：Ctrl-E Ctrl-U；其他：⌘→ ⌘⌫ | 终端（Windows Terminal / cmd / PowerShell）：Esc；其他：End、Shift+Home、Backspace |
| 权限 | 辅助功能 + 输入监控 | 不需要（目标窗口以管理员身份运行时，Windows 会拦截，属于系统限制） |

`BluetoothDictation` 只调用 `enter`、`delete_line`、`paste` 这三个语义动作，具体按键由各平台实现。

### 3.3 离座锁屏

| 功能 | Windows |
|---|---|
| 锁屏 | `LockWorkStation` |
| 是否已锁 | 是否有 `LogonUI.exe` 进程 |
| 点亮屏幕 | `SetThreadExecutionState(ES_DISPLAY_REQUIRED)` |
| 空闲秒数 | `GetLastInputInfo` |
| 鼠标微动 | `SendInput` 相对移动 +1/−1 |
| 防锁屏 | `SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED)`，由 guard 线程持有 |

**蓝牙信号强度**：WinRT 读不到已连接设备的 RSSI。读不到时 `rssi()` 返回 `None`，guard 会把它当成「断开」，延时一到就锁屏，这会误锁。所以 Windows 上把信号当作 `'usb'` 处理：只保留防锁屏和立即锁屏，与 Mac 插 USB 线时一样，胸牌上显示「USB 连接中」。使用说明里写明了这一点。

### 3.4 打开电脑程序

Windows 上用 `Get-StartApps` 列出开始菜单里的程序，桌面程序和应用商店程序都在里面，名字和开始菜单显示的一样（中文系统里就是「微信」）。打开时用 `shell:AppsFolder\<AppID>`，AppID 相当于 Mac 上的路径。匹配、别名、历史记录沿用原有逻辑；内置的英文别名（微信→WeChat）找不到时，再按说出的原词找。固件本来就支持「打开电脑的 xxx」，所以不用改固件，网页上的说法改成平台中性。

### 3.5 其他

- **单实例**：用 `msvcrt.locking` 锁住 `assistant.lock`。
- **开机启动**：写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `AI Passport Assistant` 值，内容是 `"<exe>" --background`，不需要管理员权限。
- **数据目录**：`%APPDATA%\AI Passport`。
- **编码**：Windows 默认编码是 GBK，所以打包时开启 Python 的 UTF-8 模式（`X utf8_mode=1`），源码运行时设 `PYTHONUTF8=1`，读写文件都按 UTF-8。
- **Codex**：`codex` 在 PATH 上（npm 安装的 `codex.cmd`）就能用。
- **Claude 额度**：读 `~/.claude/.credentials.json`。
- **蓝牙配对**：连上后先调用 `client.pair()`（WinRT 自动确认配对请求），再读加密特征。

## 4. 打包与 CI

`scripts/build_passport_assistant_windows.py`（在 Windows 上跑）和 `.github/workflows/windows-assistant.yml`：

1. MSYS2 UCRT64：编译 `passport-ppp.exe`，并复制它依赖的 DLL；`libopus-0.dll` 也从这里取。
2. ffmpeg：用 MSYS2 编一个最小的 ffmpeg.exe，`--enable-schannel` 换掉 SecureTransport，其余配置和 Mac 一样。
3. Python 3.12：安装依赖，运行与平台无关的单元测试。
4. PyInstaller `--onedir --windowed`，开启 UTF-8 模式。
5. 用 `--self-test` 检查依赖、Opus 编码和 PPP 网关自测，最后输出 zip 和 SHA256SUMS。

先不做安装程序：解压后双击 `AI Passport Assistant.exe` 即可，开机启动由助手自己写注册表。

## 5. 验证

| 项目 | 方式 |
|---|---|
| Mac 不回归 | 现有单元测试 + 菜单 HTTP 测试 + 真机连接 |
| 桥接数据通路 | Mac 上用桥接模式连真机：`link_ready`、模拟按键 |
| Windows 代码路径 | 单元测试注入假的 Win32 接口；CI 在 Windows 上跑测试、打包和 `--self-test` |
| Windows 真机 | 还没验证：需要一台 Windows 电脑插胸牌测试 USB 配对、语音输入粘贴和蓝牙 |

## 6. 已知限制

- Windows 上没有按信号距离锁屏（读不到 RSSI）。
- 胸牌上的少量文字仍写作「Mac」（例如「Mac 正在打开…」），要改固件，这次不做；助手回给胸牌的结果会写「电脑」。
- 管理员权限窗口收不到粘贴（UIPI），这是 Windows 的限制。
- 未做代码签名，首次运行时 SmartScreen 会提示「未知发布者」。
