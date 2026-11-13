# 多语言（i18n）：中文 / English

胸牌、手机配置页和电脑助手都支持「中文 / English」。切换方式有两种：在胸牌的「系统设置 › 语言 Language」里切换，或在手机配置页右上角切换。两处是同一个设置，保存在 NVS `suite_system/language`，0 表示中文，1 表示 English。

## 原则：只在显示时翻译

程序内部、NVS、JSON 和与电脑助手之间的协议，一律保留中文原文。文字只在显示到屏幕或网页的那一刻才换成英文。这样做的好处：
- 代码里拿中文做判断的地方，例如 `strcmp(state,"正在播放")`，不需要任何改动。
- 老数据和老版本的电脑助手照常兼容。
- 没有对应英文的文字会原样显示中文，可以逐条补翻，不会出现空白。

## 匹配规则

胸牌（`firmware/passport_suite/main/i18n.hpp`，函数 `tr()`）、手机配置页、电脑助手（网页端 `passport_i18n.js`、Python 端 `passport_i18n.py`）用同一套三步规则：

1. **整句精确匹配**：例如「已保存」→ "Saved"。
2. **按句式匹配**：
   - 胸牌直接用 printf 格式串，例如 `所在城市：%s` → `City: %s`。
   - 网页和 Python 用 `{}` 当通配符。
   - 固定文字越长的句式越先匹配。
   - 填进句式的片段如果自己有对应的英文，也会一起翻译。
   - 英文语序不同时，可以写成 `%2$s`，按位置取参数。
3. **按「 · 」拆开逐段翻译**：两侧的空格保留原样。

## 显示出口

- **胸牌**：
  - `ui.cpp` 的 `setLabel`、`home_ui.cpp` 的 `setText`、底部按键提示，统一经过 `tr()`。
  - 少数直接调用 `lv_label_set_text` 或 `strftime` 的地方，单独加了 `tr()`。
  - 状态牌在英文模式下用英文标签作大字。英文的字号是：每个单词不超过 6 个字母时用 48 px，否则用 24 px。
- **手机配置页**：用 MutationObserver 翻译新出现的文字节点和 placeholder、aria-label、title、label 属性。
  - 国内城市用拼音。
  - 海外城市用数据里自带的英文别名，国家名由浏览器的 `Intl.DisplayNames` 给出。
  - 货币名也由 `Intl.DisplayNames` 给出。
- **电脑助手**：
  - 网页加载 `/i18n.js`。
  - 原生窗口、菜单栏和 Windows 托盘的文字经过 `passport_i18n.T()`。
  - 语言跟随胸牌，胸牌通过 `/status` 的 `language` 字段报告。语言值会记在助手的配置目录里，原生窗口下次启动时生效；网页在发现胸牌是英文后立即切换。
  - 没连过胸牌时，跟随电脑系统的语言。

## 翻译表

| 文件 | 用途 |
|---|---|
| `firmware/passport_suite/i18n/en.json` | 胸牌，约 2400 条。值为 `""` 表示保留中文，用于语音关键词和数据 |
| `firmware/passport_suite/i18n/portal-en.json` | 手机配置页和菜单编辑器 |
| `firmware/passport_suite/i18n/assistant-en.json` | 电脑助手，内容叠加在 portal-en.json 之上 |

改完任一张表后运行 `python3 scripts/gen_i18n.py`。它会做这些事：
- 检查中英两边的 printf 参数是否一致。
- 检查英文里有没有残留中文。
- 检查 `{}` 的个数是否对得上。
- 生成以下文件：
  - `main/i18n_en.hpp`，按字节排序，编译时用 `static_assert` 检查顺序；
  - 手机配置页里的 `EN` 表；
  - `scripts/passport_i18n.json` 和 `scripts/passport_i18n.js`。

加上 `--todo` 参数，会把界面源码里还没收录的中文写到 `i18n/todo.json`，并附上所在的文件和行号。

**英文写法**：
- 按键名统一用 Up、Down、OK、Hold，例如 "Up/Down: select  |  OK: open  |  Hold OK: back"。
- 文字要放得下 240 px 宽的屏幕：一个汉字约等于两个英文字母的宽度。
- 聊天页和番茄钟的按键提示必须一行放下，播放语音时底部不能滚动。

## 检查

- `clang++ -std=c++20 -I firmware/passport_suite/main scripts/test_i18n.cpp -o /tmp/t && /tmp/t`：检查 `tr()` 的三步规则，并检查表里没有中文。
- `SIM_ENGLISH=1 bash scripts/build_passport_suite.sh`，再运行 `python3 scripts/test_passport_menu_sim.py --english`：
  - 整套模拟器测试用英文跑一遍，截图保存到 `releases/i18n-english-test/`。
  - 屏幕上仍显示中文的文字逐条记到 `i18n-missing.txt`，其中城市名、电台名这类内容数据是预期之内的。
- `node scripts/test_passport_portal_english.mjs`：用无头 Chrome 逐个打开手机配置页的标签页，列出剩下的中文，并检查城市、货币的英文名和语言切换。
- `python3 scripts/passport_i18n.py`：电脑助手翻译的自检。

## 保持中文的部分

- **内容数据**：电台、热榜、新闻、12306、农历和黄历、气象预警的种类、城市名（胸牌上）、股票名。英文模式下这些页面照常显示，不隐藏。
- **语音**：
  - 唤醒词「你好小智」是中文模型；换英文唤醒词需要换掉 0x670000 处的模型。
  - 记账、待办、闹钟、计算器的离线解析只认中文。
  - 「语音指令」能听懂英文的功能名，例如 "open the radio" 或 "FX rates"。
  - 其他英文说法会交给小智的大模型，由它调用胸牌的 MCP 工具完成，工具参数仍用中文。
