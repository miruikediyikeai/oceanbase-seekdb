# 全文索引第三轮优化

本轮直接对照 `f8cfb090a3ae0426b79df48065bab0bf46ac11b4`。用户提供的正式报告见 [第二轮正式反馈](../benchmarks/fts-2026-round3/submitted-round2-feedback.json)：综合提升率 2.3438%，原始性能分 4.6876/100，折合约 2.34/50，低于第一轮的 6.81/50。仓库固定 baseline 不能复现这一正式分数，正式所用 baseline 未提供，因此不推断各类别的正式提升率，也不使用仓库旧 baseline 宣称满分。

## 定位依据

在第二轮二进制上，对原基准表的 BENG 索引进行三次删除、重建，保持 20,000 行数据及原索引列和 parser 不变。使用 `perf record -a -e cpu-clock:u -F 999 -g` 采样，获得 14,290 个用户态样本，无丢样。全系统用户态采样中，排序调用链约 23.23%，`utf8mb4_general_ci` 的 Datum 比较约 12.93%，binary 比较约 7.48%，实际分词约 5.22%。这些调用链相互包含，不能相加解释为预期收益。

`get_next_batch_with_onlyone_row` 调用链包含上层索引写入，其约 69.79% 的占比不能全部归因于标量/批量转换。源码确认全文索引补数据的扫描算子主动将 `max_batch_size_` 设为零，词项逐行经过扫描和投影框架，具有可减少重复工作的具体入口。

## 修改与边界

- 全文索引补数据扫描支持同一文档内的批量词项输出。读取下一份原文档仍调用原标量路径，保留 LOB 读取、LIMIT、状态检查及 DDL 校验和累计；每批只消费同一文档，避免下次 `segment()` 重用 arena 后当前批次的词项指针失效。词项较多时跨多批排空当前文档，较少时返回短批。
- 仅四列生成结果均支持批量、没有额外过滤或 PDML 分区 ID 投影时启用。其他计划保留标量路径，采样扫描仍使用原扫描路径。不改变分词规则、词频、文档长度或词项输出次序。
- 已知字符集的非 NULL Datum 比较提前判断字节完全相等，跳过后续重复的字符集查找、参数校验和权重比较。不同字节仍调用原 collation 比较。
- binary Datum 直接按字节及长度比较，保留 NO PAD 语义，尾空格和零字节均有意义。NULL 顺序仍由原外层比较器处理，不把 `utf8mb4_bin` 等 PAD SPACE 字符集等同于 binary。

## 验证方法

新增 `fts_build_batch_regression` 的预期结果由第二轮二进制生成。测试包含 1,025 行中英文文档、NULL/空文本/停用词文本、1,300 个不同词项的单篇文档和长 LOB，覆盖 IK 与 BENG 后建索引、更新/删除、重建、分区表以及分页和相关性排序。

测试通过 MINOR FREEZE 将原文档落盘，等待非活动 memtable 消失并断言存在数据 SSTable，再构建索引，覆盖存储块读取路径。测试直接读取两种辅助表，核对所有倒排记录的数量、词频、文档长度，以及包含原始行 ID 和完整词项字节的校验和，避免只比较查询命中数量。两种辅助表均得到 5,269 条记录、词频和 10,071、文档长度和 1,722,106、posting 校验和 11,255,409,833,712。普通 SQL 覆盖 binary 的嵌入零字节、尾空格、NULL、升降序、窗口、JOIN，以及中文、重音、大小写和 PAD SPACE 排序。另使用既有 1,668 组结果差分、全文回归套件和 20,000 行普通 SQL 排序差分。

性能使用原始完整基准及原参数，采用前后版本交错运行，保留每份报告和退化项。编译、采样、正确性检查与计时分开进行；比较同一硬件和启动配置下的本地版本差异，不将本地结果换算为正式得分。基准数据、SQL、计时、评分脚本和固定 baseline 均不修改。

## 本地测量结果

环境为同一 Windows 主机上的 WSL Ubuntu 22.04 专用实例，Clang 17 Release 构建（`-O2 -g -DNDEBUG -fno-omit-frame-pointer`），客户端连接本地 10000 端口。保持相同实例资源配置，替换二进制后重启。每个版本执行三轮完整测试，每轮均重新导入、构建索引，采用 `before-1 → after-1 → after-2 → before-2 → before-3 → after-3` 顺序。计时期间暂停时间同步，测量完成后恢复。参数为：

```text
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200
SAMPLES=3 WARMUP=30 SKIP_LOAD=0
```

下表为各版本三轮报告的算术平均值，下降比例按 `(before-after)/before` 计算。所有单次报告、二进制 SHA256 和计算明细见 [测量目录](../benchmarks/fts-2026-round3/comparison.json)。

| 指标 | 第二轮 f8 | 本轮候选 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 11.546667 s | 10.043000 s | 13.02% |
| build_ik_content_sec | 8.919333 s | 7.888667 s | 11.56% |
| build_beng_en_sec | 4.517333 s | 3.847667 s | 14.82% |
| tokenize_ik_avg_ms | 0.304167 ms | 0.250467 ms | 17.65% |
| tokenize_beng_avg_ms | 0.198900 ms | 0.172900 ms | 13.07% |
| query_cn_avg_ms | 2.438500 ms | 2.445467 ms | **−0.29%** |
| query_beng_avg_ms | 3.479500 ms | 3.161867 ms | 9.13% |
| query_mixed_avg_ms | 3.110667 ms | 2.377100 ms | 23.58% |
| query_limit_avg_ms | 3.711167 ms | 3.436133 ms | 7.41% |

按三类等权计算，本地构建平均下降 13.13%，分词 15.36%，查询 9.96%，综合 12.82%。这些是相对第二轮提交的本地增量，`comparison.json` 中的公式分值同样只表示本地 A/B，不能作为正式成绩预测。四项查询的命中数量在全部六轮中保持 8,001 / 11,000 / 7,332 / 20；完整正确性由回归和结果差分另行验证。

## 采样及测量局限

最终候选的 BENG 构建采样获得 12,746 个用户态样本，无丢样，能够观察到 `inner_get_next_fts_index_batch` 调用链。排序调用链占比由 23.23% 变为 19.46%，CI Datum 比较由 12.93% 变为 10.29%，binary 比较由 7.48% 变为 2.23%，`strcmpsp` 由 6.93% 变为 3.22%。采样摘录见 [优化前](../benchmarks/fts-2026-round3/before-build-profile.txt) 和 [优化后](../benchmarks/fts-2026-round3/after-build-profile.txt)。这些是各自采样期间的相对占比，调用链相互包含，不能相加，也不能仅凭样本总数推导精确的 CPU 降幅。

分词和查询的本地墙钟波动明显，连 `SELECT 1` 的平均耗时也从 0.180567 ms 变为 0.142433 ms。本轮没有修改 parser，因此不能将 TOKENIZE 的全部表观收益归因于代码。为检查墙钟结果，另在同一份索引上，以原基准的精确文本和 SQL 执行 `旧 → 新 → 新 → 旧` 的诊断，记录整个服务进程的 CPU 增量，每版本两次，结果如下。

| 操作 | 旧版本 CPU ms/次 | 新版本 CPU ms/次 |
| --- | ---: | ---: |
| SELECT 1 | 0.1005 | 0.0870 |
| TOKENIZE IK | 0.2950 | 0.3740 |
| TOKENIZE BENG | 0.1150 | 0.2160 |
| 中文查询 | 1.765 | 1.990 |
| 英文查询 | 3.255 | 3.175 |
| 混合查询 | 2.220 | 2.290 |
| LIMIT 查询 | 3.305 | 2.990 |

[CPU 诊断原始记录](../benchmarks/fts-2026-round3/cpu-diagnostics.json) 包含每次结果哈希，前后均一致。整进程 CPU 包含后台线程，且两次样本波动较大，不能等同于单条 SQL 的执行线程 CPU；但它也没有支持分词和全部查询均获益的结论，因此保留其退化数据。当前较可信的增量证据集中在三项构建：完整测试均值下降约 12%–15%，同时采样显示比较热点减少。查询和分词收益仍需正式环境确认。

## 最终验证记录

Release 编译及最后一次定向重编、归档、链接均成功。最终测量二进制 SHA256 为 `9070103486318f9be18818e9951b586152f38b826bc467da38323cb40db23f3b`，四个源码文件在 Windows 提交工作区和 Linux 编译目录逐字节一致，见 [核验记录](../benchmarks/fts-2026-round3/verification.json)。

以下九套 mysqltest 全部通过：`fts_build_batch_regression`、`fts_batch_merge_regression`、`fts_hotpath_regression`、`tokenize_function`、`simple_query`、`parser_properties`、`domain_id_merge_with_compare_null`、`create_table_with_fts_index`、`create_fts_index_afterward`。最终候选通过 1,668 组结果差分及普通 SQL 的全排序、TopN、窗口和聚合差分；候选构建的索引也由旧二进制重新读取核对通过。[验证日志](../benchmarks/fts-2026-round3/validation.txt) 保留最终候选的构建与测试结果。
