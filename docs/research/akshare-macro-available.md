# AKShare 宏观免费上游可用清单

当前选定范围：[中国、美国、商品三类](akshare-macro-cn-us-commodities.md)，共 74 个独立接口；本页保留完整核验目录。

核验日期：2026-10-30。覆盖宏观目录 215 个公开数据入口（排除 5 个 core 辅助函数），以及相关央行、同业拆借、回购、国债 15 个入口，共 230 个。

确认可用于主机采集 118 个；从白名单排除数据过期 101 个、访问或业务报错 2 个、未验证 9 个。

“可用”表示匿名 HTTP 可访问、响应可解析、有非空实际数据，且数据日期通过按频率设置的时效检查。只取一页检查；不代表全部历史、所有参数组合、统计口径正确性或 ESP32 实机测试通过。尚未将这些接口加入固件。

所有请求均不需要个人账号、浏览器个人 Cookie 或付费 API Key。公开 token/x-app-id 是上游网站参数。AKShare 本身不需要运行；白名单保存的是可移植的底层 HTTP 请求。

日频最多容许 14 天，周频 35 天，月频 95 天，季频及政策事件 190 天；加拿大月度 GDP 考虑常规公布滞后容许 140 天，年度旅游数据 730 天。政策事件超期只表示当前性未确认，不代表服务永久失效。未来日期及空“今值”不作为有效实际观测。年/月/季度日期多为所属期标记，不能当作发布日期。

表中每行算一个 AKShare 函数入口。一个入口可能返回多个指标，多源重复数据也保留各自入口，因此接口数量不等于独立指标数量。`rate_interbank` 本轮仅验证默认 Shibor 隔夜，`repo_rate_query` 仅验证默认回购定盘利率；未将所有市场、币种和期限组合认定可用。

## 全部可用入口

| 类别 | 接口数 |
| --- | ---: |
| 中美国债 | 1 |
| 银行间利率 | 1 |
| 回购利率 | 2 |
| 澳大利亚 | 4 |
| 加拿大 | 9 |
| 中国内地、行业及航运 | 52 |
| 中国香港 | 7 |
| 全球 ETF | 2 |
| 德国 | 7 |
| 财经日历 | 1 |
| 日本 | 5 |
| 瑞士 | 5 |
| 英国 | 14 |
| 美国及相关机构 | 8 |

## 中美国债（1）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `bond_zh_us_rate` | 中美国债收益率 | 2026-10-29 |

## 银行间利率（1）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `rate_interbank` | 拆借利率一览-具体市场的具体品种的具体指标的拆借利率数据 | 2026-10-30 |

## 回购利率（2）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `repo_rate_query` | 中国外汇交易中心暨全国银行间同业拆借中心-回购定盘利率-历史数据 | 2026-10-30 |
| `repo_rate_hist` | 中国外汇交易中心暨全国银行间同业拆借中心-回购定盘利率-历史数据 | 2026-10-30 |

## 澳大利亚（4）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_australia_unemployment_rate` | 澳大利亚-失业率 | 2026-09-01 |
| `macro_australia_ppi_quarterly` | 澳大利亚-生产者物价指数季率 | 2026-09-01 |
| `macro_australia_cpi_quarterly` | 澳大利亚-消费者物价指数季率 | 2026-09-01 |
| `macro_australia_cpi_yearly` | 澳大利亚-消费者物价指数年率 | 2026-09-01 |

## 加拿大（9）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_canada_new_house_rate` | 加拿大-新屋开工 | 2026-09-01 |
| `macro_canada_unemployment_rate` | 加拿大-失业率 | 2026-09-01 |
| `macro_canada_trade` | 加拿大-贸易帐 | 2026-08-01 |
| `macro_canada_bank_rate` | 加拿大-央行公布利率决议 | 2026-10-01 |
| `macro_canada_core_cpi_yearly` | 加拿大-核心消费者物价指数年率 | 2026-09-01 |
| `macro_canada_core_cpi_monthly` | 加拿大-核心消费者物价指数月率 | 2026-09-01 |
| `macro_canada_cpi_yearly` | 加拿大-消费者物价指数年率 | 2026-09-01 |
| `macro_canada_cpi_monthly` | 加拿大-消费者物价指数月率 | 2026-09-01 |
| `macro_canada_gdp_monthly` | 加拿大-GDP 月率 | 2026-07-01 |

## 中国内地、行业及航运（52）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_china_qyspjg` | 中国-企业商品价格指数 | 2026-09-01 |
| `macro_china_shibor_all` | 上海银行业同业拆借报告 | 2026-10-30 |
| `macro_china_hk_market_info` | 香港同业拆借报告 | 2026-10-29 |
| `macro_china_market_margin_sz` | 深圳融资融券报告 | 2026-10-29 |
| `macro_china_market_margin_sh` | 上海融资融券报告 | 2026-10-29 |
| `macro_china_au_report` | 上海黄金交易所报告 | 2026-10-29 |
| `macro_china_lpr` | LPR品种详细数据 | 2026-10-20 |
| `macro_china_new_house_price` | 中国-新房价指数 | 2026-09-01 |
| `macro_china_enterprise_boom_index` | 企业景气及企业家信心指数 | 2026-06-01 |
| `macro_china_national_tax_receipts` | 中国-全国税收收入 | 2026（年） |
| `macro_china_bank_financing` | 银行理财产品发行数量 | 2026-09-01 |
| `macro_china_insurance_income` | 原保险保费收入 | 2026-09-01 |
| `macro_china_mobile_number` | 手机出货量 | 2026-09-01 |
| `macro_china_vegetable_basket` | 菜篮子产品批发价格指数 | 2026-10-29 |
| `macro_china_agricultural_product` | 农产品批发价格总指数 | 2026-10-29 |
| `macro_china_agricultural_index` | 农副指数 | 2026-10-29 |
| `macro_china_energy_index` | 能源指数 | 2026-10-29 |
| `macro_china_commodity_price_index` | 大宗商品价格 | 2026-10-29 |
| `macro_global_sox_index` | 费城半导体指数 | 2026-10-29 |
| `macro_china_construction_index` | 建材指数 | 2026-10-29 |
| `macro_china_lpi_index` | 物流景气指数 | 2026-09-01 |
| `macro_china_bdti_index` | 原油运输指数 | 2026-10-29 |
| `macro_china_bsi_index` | 超灵便型船运价指数 | 2026-10-29 |
| `macro_shipping_bci` | 海岬型运费指数(BCI) | 2026-10-29 |
| `macro_shipping_bdi` | 波罗的海干散货指数(BDI) | 2026-10-29 |
| `macro_shipping_bpi` | 巴拿马型运费指数(BPI) | 2026-10-29 |
| `macro_shipping_bcti` | 成品油运输指数（BCTI） | 2026-10-29 |
| `macro_china_new_financial_credit` | 中国-新增信贷数据 | 2026-09-01 |
| `macro_china_fx_gold` | 外汇和黄金储备 | 2026-09-01 |
| `macro_china_stock_market_cap` | 全国股票交易统计表 | 2026-10-01 |
| `macro_china_money_supply` | 货币供应量 | 2026-09-01 |
| `macro_china_cpi` | 中国居民消费价格指数 | 2026-09-01 |
| `macro_china_gdp` | 中国国内生产总值 | 2026-09-01 |
| `macro_china_ppi` | 中国工业品出厂价格指数 | 2026-09-01 |
| `macro_china_pmi` | 中国采购经理人指数 | 2026-09-01 |
| `macro_china_gdzctz` | 中国城镇固定资产投资 | 2026-09-01 |
| `macro_china_hgjck` | 海关进出口增减情况一览表 | 2026-09-01 |
| `macro_china_czsr` | 财政收入 | 2026-09-01 |
| `macro_china_whxd` | 外汇贷款数据 | 2026-09-01 |
| `macro_china_wbck` | 本外币存款 | 2026-09-01 |
| `macro_china_xfzxx` | 消费者信心指数 | 2026-08-01 |
| `macro_china_gyzjz` | 工业增加值增长 | 2026-09-01 |
| `macro_china_consumer_goods_retail` | 社会消费品零售总额 | 2026-09-01 |
| `macro_china_society_electricity` | 全社会用电分类情况表 | 2026-09-01 |
| `macro_china_society_traffic_volume` | 全社会客货运输量 | 2026-09-01 |
| `macro_china_postal_telecommunicational` | 邮电业务基本情况 | 2026-09-01 |
| `macro_china_international_tourism_fx` | 国际旅游外汇收入构成 | 2025（年） |
| `macro_china_passenger_load_factor` | 民航客座率及载运率 | 2026-09-01 |
| `macro_china_central_bank_balance` | 央行货币当局资产负债 | 2026-09-01 |
| `macro_china_insurance` | 保险业经营情况 | 2026-09-01 |
| `macro_china_supply_of_money` | 货币供应量 | 2026-09-01 |
| `macro_china_foreign_exchange_gold` | 央行黄金和外汇储备 | 2026-09-01 |

## 中国香港（7）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_china_hk_rate_of_unemployment` | 中国香港-失业率 | 2026-09-01 |
| `macro_china_hk_gbp` | 中国香港-香港 GDP | 2026-06-01 |
| `macro_china_hk_gbp_ratio` | 中国香港-香港 GDP 同比 | 2026-06-01 |
| `macro_china_hk_building_volume` | 中国香港-香港楼宇买卖合约数量 | 2026-09-01 |
| `macro_china_hk_building_amount` | 中国香港-香港楼宇买卖合约成交金额 | 2026-09-01 |
| `macro_china_hk_trade_diff_ratio` | 中国香港-香港商品贸易差额年率 | 2026-09-01 |
| `macro_china_hk_ppi` | 中国香港-香港制造业 PPI 年率 | 2026-06-01 |

## 全球 ETF（2）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_cons_gold` | 黄金 ETF 持仓 | 2026-10-29 |
| `macro_cons_silver` | 白银 ETF 持仓 | 2026-10-28 |

## 德国（7）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_germany_ifo` | 德国-IFO商业景气指数 | 2026-10-01 |
| `macro_germany_cpi_monthly` | 德国-消费者物价指数月率终值 | 2026-09-01 |
| `macro_germany_cpi_yearly` | 德国-消费者物价指数年率终值 | 2026-09-01 |
| `macro_germany_trade_adjusted` | 德国-贸易帐（季调后） | 2026-08-01 |
| `macro_germany_gdp` | 德国-GDP | 2026-06-01 |
| `macro_germany_retail_sale_monthly` | 德国-实际零售销售月率 | 2026-09-01 |
| `macro_germany_zew` | 德国-ZEW 经济景气指数 | 2026-10-01 |

## 财经日历（1）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_info_ws` | 华尔街见闻-日历-宏观 | 2026-10-30 |

## 日本（5）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_japan_bank_rate` | 日本-央行公布利率决议 | 2026-10-30 |
| `macro_japan_cpi_yearly` | 日本-全国消费者物价指数年率 | 2026-09-01 |
| `macro_japan_core_cpi_yearly` | 日本-全国核心消费者物价指数年率 | 2026-09-01 |
| `macro_japan_unemployment_rate` | 日本-失业率 | 2026-09-01 |
| `macro_japan_head_indicator` | 日本-领先指标终值 | 2026-08-01 |

## 瑞士（5）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_swiss_svme` | 瑞士-SVME采购经理人指数 | 2026-09-01 |
| `macro_swiss_trade` | 瑞士-贸易帐 | 2026-09-01 |
| `macro_swiss_gdp_quarterly` | 瑞士-GDP季率 | 2026-06-01 |
| `macro_swiss_gbd_yearly` | 瑞士-GDP 年率 | 2026-06-01 |
| `macro_swiss_gbd_bank_rate` | 瑞士-央行公布利率决议 | 2026-09-01 |

## 英国（14）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_uk_halifax_monthly` | 英国-Halifax 房价指数月率 | 2026-09-01 |
| `macro_uk_halifax_yearly` | 英国-Halifax 房价指数年率 | 2026-09-01 |
| `macro_uk_trade` | 英国-贸易帐 | 2026-08-01 |
| `macro_uk_bank_rate` | 英国-央行公布利率决议 | 2026-09-01 |
| `macro_uk_core_cpi_yearly` | 英国-核心消费者物价指数年率 | 2026-09-01 |
| `macro_uk_core_cpi_monthly` | 英国-核心消费者物价指数月率 | 2026-09-01 |
| `macro_uk_cpi_yearly` | 英国-消费者物价指数年率 | 2026-09-01 |
| `macro_uk_cpi_monthly` | 英国-消费者物价指数月率 | 2026-09-01 |
| `macro_uk_retail_monthly` | 英国-零售销售月率 | 2026-09-01 |
| `macro_uk_retail_yearly` | 英国-零售销售年率 | 2026-09-01 |
| `macro_uk_rightmove_yearly` | 英国-Rightmove 房价指数年率 | 2026-10-01 |
| `macro_uk_rightmove_monthly` | 英国-Rightmove 房价指数月率 | 2026-10-01 |
| `macro_uk_gdp_quarterly` | 英国-GDP 季率初值 | 2026-06-01 |
| `macro_uk_unemployment_rate` | 英国-失业率 | 2026-08-01 |

## 美国及相关机构（8）

| AKShare 接口 | 数据内容 | 最近有效所属日期/期 |
| --- | --- | --- |
| `macro_usa_phs` | 美国-未决房屋销售月率 | 2026-09-01 |
| `macro_usa_cpi_yoy` | 美国-CPI年率 | 2026-09-01 |
| `macro_usa_crude_inner` | 美国原油产量报告 | 2026-10-23 |
| `macro_usa_cftc_nc_holding` | 美国商品期货交易委员会CFTC外汇类非商业持仓报告 | 2026-10-20 |
| `macro_usa_cftc_c_holding` | 美国商品期货交易委员会CFTC商品类非商业持仓报告 | 2026-10-20 |
| `macro_usa_cftc_merchant_currency_holding` | 美国商品期货交易委员会CFTC外汇类商业持仓报告 | 2026-10-20 |
| `macro_usa_cftc_merchant_goods_holding` | 美国商品期货交易委员会CFTC商品类商业持仓报告 | 2026-10-20 |
| `macro_usa_cme_merchant_goods_holding` | CME-贵金属 | 2026-10-29 |

## 请求参数及验证证据

- [活动白名单与完整请求参数](akshare-macro-available.json)：URL、GET/POST、query/body、公开请求头、来源代码和响应摘要。
- [排除清单](akshare-macro-excluded.json)：101 个过期、2 个报错、9 个未验证入口，逐项保留原因。
- [完整核验记录](akshare-macro-audit.json)：全部 230 个入口、最新样本、字节数、时效阈值与原始响应 SHA-256。
- 原始响应留在本项目忽略的 `releases/akshare-macro-audit/raw/`，不会作为固件资源打包。

其中 2 个报错为 `macro_china_bond_public`（匿名访问 HTTP 403）和 `macro_china_swap_rate`（HTTP 200，但上游业务 rep_code=500）。没有把 HTTP 200 当成成功的充分条件。

未验证的 9 个入口：国家统计局 3 个（浏览器兼容会话）、百度日历 1 个（会话/Cookie 辅助链）、Excel/HTML 采集 5 个。这些入口不在活动白名单，也不被标注为已证明永久失效。

美国 GDP、非农、核心 PCE 等金十入口和欧元区宏观、OPEC、LME 等过期项已经排除。香港仍有失业、GDP、楼宇、贸易、PPI 可用，不能因香港 CPI 过期就排除整个地区。

直连移植还需要处理：JSONP/GB18030（新浪）、分页/响应大小（部分金十 CDN）、单位与日期，以及 ESP32 的 TLS/内存。白名单保留采集可用入口，不表示所有入口适合第一批固件接入。

## 复现

```sh
python3 scripts/audit_akshare_macro.py \
  --source releases/akshare-macro-audit/source \
  --output docs/research/akshare-macro-audit.json \
  --raw releases/akshare-macro-audit/raw
```

源码固定为 `62a56b2159add40f41b2c19129be3ac2af297d09`；脚本不导入或执行 AKShare，只解析源码中的请求构造，并发上限 3，仅采样一页。缺少源码文件时从固定版本下载。采集脚本更新完整核验记录，白名单为本次核验快照；重新采集后应按 `status=available` 重建名单。

官方资料：[宏观目录](https://akshare.akfamily.xyz/data/macro/macro.html)、[美国接口停更说明](https://akshare.akfamily.xyz/data/macro/macro_usa.html)、[其他国家](https://akshare.akfamily.xyz/data/macro/macro_other.html)、[中国宏观](https://akshare.akfamily.xyz/data/macro/macro_china.html)。

## 本轮检查

脚本语法、118 份保留响应的 SHA-256/再解析/日期时效、白名单与排除名单互斥及完整覆盖检查通过。路由覆盖检查通过；完整路由校验因 Git 未追踪文件失败，包括本轮新增研究文件和原有宏观源码/文档/测试。没有修改暂存区或放宽校验规则。
