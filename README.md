# Ghost Flow Stats · 流量历史

Windows x64 原生 C++ 外部插件。可在 Ghost Proxifier 插件中心直接安装，或开启开发者模式后从本地导入 `com.ghostproxifier.flow-stats-1.0.0.gpkg`，启用插件并授予 `stats.read` 和 `config.read`。已安装旧版时可直接升级，从 1.3.1 或更早版本升级时需确认新增的配置读取权限。

启用后，每 5 秒采样 Ghost 的 `GET /process-stats`，把逐进程 `up/down` 的增量归并到本地 SQLite。该口径和 Ghost 流量页一致。关闭仪表盘时仍会采集，数据库位于插件私有数据目录下的 `traffic.sqlite3`。

顶部时间范围统一筛选总量、趋势柱图和排行。可选今日或最近 7、14、30、60、180 天，包含今天，默认 14 天。选择今日时柱图按每小时展示；长范围的每日柱图可横向滚动。进程排行按进程名合并。上游节点排行按 `stats[].node` 的上报名称合并，同名算作一项，跳过未上报节点、直连、拦截和不可用的占位值。两个排行都分别显示上传、下载的前 10 名。

- 周期对比使用所选日期范围之前连续的等长区间。总量卡显示上期基数及变化，变化列表显示上传和下载合计变动最大的进程与节点。
- 节点配额按本地自然月统计上传和下载合计。设置配额时从 Ghost 脱敏配置选择节点，再填写每月 GB 额度（1 GB = 10 亿字节）。每个节点的卡片显示已用、剩余和累计曲线。实际用量以供应商后台为准。
- 日内热力图按所选范围内的星期和本地小时汇总。小时数据从 1.3.0 开始记录。

升级保留旧版进程和每日总量。旧版把 UDP 计数额外相加，部分流量可能重复，已落盘的总量无法拆回。节点榜自 1.1.0 开始采集，旧记录没有节点名称可供回填。

这是采样统计。插件停用期间不会采集，已经在运行的进程在首个样本建立基线。采样间隔内退出的进程、最后一个样本之后的流量，以及宿主未上报的字节都可能缺失。跨午夜的样本增量记在采样当天。

## 构建

需要 Visual Studio 2022 C++ 工具、Windows SDK 和 Python。SQLite 使用 Windows 的 `winsqlite3.dll`。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix build/stage
python tools/plugin/gpkg.py pack --src build/stage --out build/dist --min-app-version 1.2.2
```
