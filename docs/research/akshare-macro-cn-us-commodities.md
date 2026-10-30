# 中国、美国、商品：免费宏观接口筛选

基于 2026-10-30 已验证的 AKShare 免费上游白名单，共保留 **74 个独立接口**。本轮只筛选和分类，没有重新联网检测或接入固件。

| 类别 | 可用入口数 | 范围 |
| --- | ---: | --- |
| 中国 | 49 | 内地及香港宏观、金融、行业，中债及中国财经日历 |
| 美国 | 7 | CPI、房地产、半导体指数、外汇持仓、美债及美国财经日历 |
| 商品 | 20 | 商品价格、农产品、能源、原油产量、贵金属、商品持仓、航运 |

国债与财经日历各由一个接口同时服务中美两类，因此分类计数相加为 76，去重后为 74。一个接口可以提供多个指标；重复来源保留各自入口。

商品按数据内容归类。例如美国原油产量进入商品类；黄金与外汇储备仍作为中国央行宏观数据。商品类包含航运等供需相关指标。

其他国家和地区的 44 个入口不在本次选定范围。原来已剔除的过期、报错及未验证入口不会进入本清单。

## 中国（49）

| AKShare 接口 | 数据内容 | 最近有效所属期 | 说明 |
| --- | --- | --- | --- |
| `bond_zh_us_rate` | 中国 2/5/10/30 年国债收益率与 10Y-2Y 利差 | 2026-10-29 | 按中/美国债字段分开；请求和响应共用 |
| `rate_interbank` | 拆借利率一览-具体市场的具体品种的具体指标的拆借利率数据 | 2026-10-30 | 仅验证 Shibor 隔夜参数 |
| `repo_rate_query` | 中国外汇交易中心暨全国银行间同业拆借中心-回购定盘利率-历史数据 | 2026-10-30 | 仅验证默认回购定盘利率 CSV |
| `repo_rate_hist` | 中国外汇交易中心暨全国银行间同业拆借中心-回购定盘利率-历史数据 | 2026-10-30 |  |
| `macro_china_shibor_all` | 上海银行业同业拆借报告 | 2026-10-30 |  |
| `macro_china_hk_market_info` | 香港同业拆借报告 | 2026-10-29 |  |
| `macro_china_market_margin_sz` | 深圳融资融券报告 | 2026-10-29 |  |
| `macro_china_market_margin_sh` | 上海融资融券报告 | 2026-10-29 |  |
| `macro_china_lpr` | LPR品种详细数据 | 2026-10-20 |  |
| `macro_china_new_house_price` | 中国-新房价指数 | 2026-09-01 |  |
| `macro_china_enterprise_boom_index` | 企业景气及企业家信心指数 | 2026-06-01 |  |
| `macro_china_national_tax_receipts` | 中国-全国税收收入 | 2026-09-01 |  |
| `macro_china_bank_financing` | 银行理财产品发行数量 | 2026-09-01 |  |
| `macro_china_insurance_income` | 原保险保费收入 | 2026-09-01 |  |
| `macro_china_mobile_number` | 手机出货量 | 2026-09-01 |  |
| `macro_china_lpi_index` | 物流景气指数 | 2026-09-01 |  |
| `macro_china_new_financial_credit` | 中国-新增信贷数据 | 2026-09-01 |  |
| `macro_china_fx_gold` | 外汇和黄金储备 | 2026-09-01 |  |
| `macro_china_stock_market_cap` | 全国股票交易统计表 | 2026-10-01 |  |
| `macro_china_money_supply` | 货币供应量 | 2026-09-01 |  |
| `macro_china_cpi` | 中国居民消费价格指数 | 2026-09-01 |  |
| `macro_china_gdp` | 中国国内生产总值 | 2026-09-01 |  |
| `macro_china_ppi` | 中国工业品出厂价格指数 | 2026-09-01 |  |
| `macro_china_pmi` | 中国采购经理人指数 | 2026-09-01 |  |
| `macro_china_gdzctz` | 中国城镇固定资产投资 | 2026-09-01 |  |
| `macro_china_hgjck` | 海关进出口增减情况一览表 | 2026-09-01 |  |
| `macro_china_czsr` | 财政收入 | 2026-09-01 |  |
| `macro_china_whxd` | 外汇贷款数据 | 2026-09-01 |  |
| `macro_china_wbck` | 本外币存款 | 2026-09-01 |  |
| `macro_china_xfzxx` | 消费者信心指数 | 2026-08-01 |  |
| `macro_china_gyzjz` | 工业增加值增长 | 2026-09-01 |  |
| `macro_china_consumer_goods_retail` | 社会消费品零售总额 | 2026-09-01 |  |
| `macro_china_society_electricity` | 全社会用电分类情况表 | 2026-09-01 |  |
| `macro_china_society_traffic_volume` | 全社会客货运输量 | 2026-09-01 |  |
| `macro_china_postal_telecommunicational` | 邮电业务基本情况 | 2026-09-01 |  |
| `macro_china_international_tourism_fx` | 国际旅游外汇收入构成 | 2025（年） | 年频，2025 年数据 |
| `macro_china_passenger_load_factor` | 民航客座率及载运率 | 2026-09-01 |  |
| `macro_china_central_bank_balance` | 央行货币当局资产负债 | 2026-09-01 |  |
| `macro_china_insurance` | 保险业经营情况 | 2026-09-01 |  |
| `macro_china_supply_of_money` | 货币供应量 | 2026-09-01 |  |
| `macro_china_foreign_exchange_gold` | 央行黄金和外汇储备 | 2026-09-01 |  |
| `macro_china_hk_rate_of_unemployment` | 中国香港-失业率 | 2026-09-01 |  |
| `macro_china_hk_gbp` | 中国香港-香港 GDP | 2026-06-01 |  |
| `macro_china_hk_gbp_ratio` | 中国香港-香港 GDP 同比 | 2026-06-01 |  |
| `macro_china_hk_building_volume` | 中国香港-香港楼宇买卖合约数量 | 2026-09-01 |  |
| `macro_china_hk_building_amount` | 中国香港-香港楼宇买卖合约成交金额 | 2026-09-01 |  |
| `macro_china_hk_trade_diff_ratio` | 中国香港-香港商品贸易差额年率 | 2026-09-01 |  |
| `macro_china_hk_ppi` | 中国香港-香港制造业 PPI 年率 | 2026-06-01 |  |
| `macro_info_ws` | 中国财经日历 | 2026-10-30 | 仅取本类国家事件，并排除次日边界记录 |

## 美国（7）

| AKShare 接口 | 数据内容 | 最近有效所属期 | 说明 |
| --- | --- | --- | --- |
| `bond_zh_us_rate` | 美国 2/5/10/30 年国债收益率与 10Y-2Y 利差 | 2026-10-29 | 按中/美国债字段分开；请求和响应共用 |
| `macro_global_sox_index` | 费城半导体指数 | 2026-10-29 | 费城半导体指数，归美国 |
| `macro_info_ws` | 美国财经日历 | 2026-10-30 | 仅取本类国家事件，并排除次日边界记录 |
| `macro_usa_phs` | 美国-未决房屋销售月率 | 2026-09-01 |  |
| `macro_usa_cpi_yoy` | 美国-CPI年率 | 2026-09-01 |  |
| `macro_usa_cftc_nc_holding` | 美国商品期货交易委员会CFTC外汇类非商业持仓报告 | 2026-10-20 |  |
| `macro_usa_cftc_merchant_currency_holding` | 美国商品期货交易委员会CFTC外汇类商业持仓报告 | 2026-10-20 |  |

## 商品（20）

| AKShare 接口 | 数据内容 | 最近有效所属期 | 说明 |
| --- | --- | --- | --- |
| `macro_china_qyspjg` | 中国-企业商品价格指数 | 2026-09-01 |  |
| `macro_china_au_report` | 上海黄金交易所报告 | 2026-10-29 |  |
| `macro_china_vegetable_basket` | 菜篮子产品批发价格指数 | 2026-10-29 |  |
| `macro_china_agricultural_product` | 农产品批发价格总指数 | 2026-10-29 |  |
| `macro_china_agricultural_index` | 农副指数 | 2026-10-29 |  |
| `macro_china_energy_index` | 能源指数 | 2026-10-29 |  |
| `macro_china_commodity_price_index` | 大宗商品价格 | 2026-10-29 |  |
| `macro_china_construction_index` | 建材指数 | 2026-10-29 |  |
| `macro_china_bdti_index` | 原油运输指数 | 2026-10-29 |  |
| `macro_china_bsi_index` | 超灵便型船运价指数 | 2026-10-29 |  |
| `macro_shipping_bci` | 海岬型运费指数(BCI) | 2026-10-29 |  |
| `macro_shipping_bdi` | 波罗的海干散货指数(BDI) | 2026-10-29 |  |
| `macro_shipping_bpi` | 巴拿马型运费指数(BPI) | 2026-10-29 |  |
| `macro_shipping_bcti` | 成品油运输指数（BCTI） | 2026-10-29 |  |
| `macro_cons_gold` | 黄金 ETF 持仓 | 2026-10-29 |  |
| `macro_cons_silver` | 白银 ETF 持仓 | 2026-10-28 |  |
| `macro_usa_crude_inner` | 美国原油产量报告 | 2026-10-23 |  |
| `macro_usa_cftc_c_holding` | 美国商品期货交易委员会CFTC商品类非商业持仓报告 | 2026-10-20 |  |
| `macro_usa_cftc_merchant_goods_holding` | 美国商品期货交易委员会CFTC商品类商业持仓报告 | 2026-10-20 |  |
| `macro_usa_cme_merchant_goods_holding` | CME-贵金属 | 2026-10-29 |  |

## 数据与范围

- [本次筛选 JSON](akshare-macro-cn-us-commodities.json)：74 个接口、原请求参数、数据来源、响应摘要及中美共用接口的字段/国家限制。
- [完整可用白名单](akshare-macro-available.md)：之前确认的全部 118 个入口。
- [原排除清单](akshare-macro-excluded.json)：继续排除的过期、报错及未验证入口。

以上是接口筛选时的主机采集快照。统计口径、全历史和所有参数组合没有逐项验证，主机结果不能代替ESP32验证。后续固件0.5.21已接入中国、美国、商品各六项，使用FRED、东方财富和金十免费公开端点；实际指标、按键及独立实机验证见 [宏观雷达](../macro-radar.md)。

本轮已检查：74 个入口均来自原白名单、未混入排除项、原请求与验证证据保持一致、分类计数正确、国债字段和日历国家过滤与原始响应相符。路由覆盖检查通过；完整路由校验仍因原有宏观文件和新增研究文件尚未被 Git 跟踪而未通过。
