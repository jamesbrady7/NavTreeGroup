# PROJECT.md —— NavTreeGroup 引擎事实基线(多 agent 交叉验证用)

> **这份文档的用途**:多个 agent(实现方 / 验证方)与人,基于**同一份事实基线**迭代,
> 避免"各自读一遍代码、各自得出不同结论"。
>
> **写作纪律(必须遵守)**:
> 1. 每条断言都要带 `文件:行` 或**可复跑的命令/探针**,不许写"应该"、"大概"。
> 2. 每条断言标注**验证状态**(见下),不许把"理论上安全"写成"实测安全"。
> 3. 改代码的人**必须**同步改这里的对应条目;验证方以这份文档为准复验。
> 4. ⚠ **行号会漂移**:本仓库有多个 agent 并行改动,`文件:行`引用在每次验证前
>    **必须重新核对**(首版发布 10 分钟内就有 3 处因 `tests.cpp` 被改动而失效)。
>    引用代码时优先写**函数名/符号名**,行号只作辅助。
>
> **验证状态标记**:
> `[实测]` = 跑过命令/探针,有输出证据 · `[读码]` = 只做过源码审读,未独立运行验证
> · `[论证]` = 只有推理,无实证(必须写出"为什么暂时无法实证")
>
> 最后更新:**release 分支:测试提速(规模两档)+ `Pipeline`→`ProjectionStages` 改名 + 行覆盖率 94.3%**(2026-10-07)。此前同日:**回收粒度修复(探针 P4 实证)** —— C3 改写为"两个回收粒度必须匹配
> 坐标作废的范围"(`collectGarbage` 只给 reset 通道 / `collectGarbageSince` 给 removeRows 通道),
> C2 修订(`discard` 退出"立即释放",改延迟),`NavTreeModel::endRowRemoval` 改为按水位回收;
> 新增 **T25**(closeMembership 纯函数)与 **P4** 探针。此前同日:**空壳组修复 + 对接契约修正** ——
> 新增不变量 #11(成员表闭合 / `closeMembership` / T24)、C5 补"属性漂移无兜底"、
> §1.5 补异常窗口、§1.1 补通知 API、§3 补 paranoid 首发纪律;并修掉三处 v4 遗留的文档漂移。
> 此前:**v4 重构(2026-10-07)** 脏通知契约取代惰性发现,设计全文见 **docs/重构v4_脏通知.md**,
> 性能/内存对比见 §3,契约变化见 C5(v4)/C8(作废);
> 第三方评审(2026-10-06)修复 R1/R2/R3;实现方(第三轮)§5 缺口全关闭。
> 改动请在此追加一行"变更记录"。

---

## 0. 快速事实

| 项 | 值 |
|---|---|
| 目标 | 把数据侧三政体原始树,"惰性投影"成带分组/过滤/排序的展示树 |
| **接入手册(给内网)** | **docs/部署手册.md** —— 场景化代码(最小接线 / 全量重建 / 释放全部孩子 / 精准删除 / 属性变更 / 换规则 / 过滤搜索 / 多根),每段都配"该调什么·不该调什么·为什么";片段由 `tools/manual_snippets_check.cpp` 做 API 检查,接口改名即编不过 |
| 语言/工具链 | C++17 · MSVC(VS2019, 14.29) · Qt 5.15.2 msvc2019_64 · CMake ≥3.16 VS 生成器 |
| 源码规模 | 非 build 文件 28 个;引擎核心 4 个文件(treeprojection.h/.cpp、projection_stages.h、ptrset.h) |
| 构建产物 | 7 个可执行:`navtree_demo` `gui_demo` `tree_tabs` `unit_tests` `interactive_demo` `bench` `probe_review`(评审探针) |
| **不做 add_library** | 决策:内网**直接集成源码**,不抽静态库。(本项已定,勿再提议) |
| 集成硬依赖 | `unit_tests` **需要 Qt5::Test**(T18 用了 `QAbstractItemModelTester`);其余 5 个目标只依赖 Core(2 个 GUI 目标额外 Widgets) |
| Debug 自检行为 | `unit_tests` 的 `main` 已调 `silenceCrtDialogs()`(`tests.cpp`):`_CrtSetReportMode` **逐类型**设 `_CRTDBG_MODE_FILE` + `SetErrorMode(...)`。**Debug 版跑测试不再弹 MessageBox** —— 此前堆损坏/QFatal 会弹框阻塞,使"退出码=失败数"契约失效并盖住现场。⚠ 若看到弹框,说明有人回退了这个函数 |

### 复现命令(整套回归)

```powershell
$env:PATH="D:\Qt\5.15.2\msvc2019_64\bin;$env:PATH"
$env:QT_QPA_PLATFORM="offscreen"
$P="E:\Files\Codes\2026\QtCodes\NavTreeGroup"
D:\CMAKE\bin\cmake.exe -S $P -B "$P\build" -G "Visual Studio 16 2019" -A x64   # CMakeLists 改动后必须重跑
D:\CMAKE\bin\cmake.exe --build "$P\build" --config Release
D:\CMAKE\bin\cmake.exe --build "$P\build" --config Debug
& "$P\build\Release\unit_tests.exe"                 # 退出码 = 失败块数,应为 0
& "$P\build\Release\navtree_demo.exe"
& "$P\build\Release\gui_demo.exe"
& "$P\build\Release\tree_tabs.exe" --selftest
& "$P\build\Release\interactive_demo.exe" --selftest
& "$P\build\Release\bench.exe"                      # 快档(默认):形状不变、规模缩小,< 1 s
& "$P\build\Release\bench.exe" full                 # ★原规模(1,010,101 节点):§3 的绝对读数出自这一档,约 1 min
& "$P\build\Release\bench.exe" deep                 # 10 万深链,进程隔离
& "$P\build\Release\bench.exe" dbg
& "$P\build\Release\bench.exe" cold,prune    # ★依赖账本的场景必须带 cold 前缀,见下
& "$P\build\Release\bench.exe" cold,mem      # 账本内存分解(含延迟释放)  ← 有前置依赖,见下
& "$P\build\Release\bench.exe" cold,regroup  # C4 实证:换规则 → 延迟释放峰值      ← 有前置依赖
& "$P\build\Release\bench.exe" gcstress    # 内存安全压测(Release 只查泄漏)
& "$P\build\Debug\bench.exe"   gcstress    # ★Debug 版才有 _CrtCheckMemory 堆校验
& "$P\build\Release\probe_review.exe"      # 评审探针:P1~P4 四疑点,期望 summary: 0 confirmed
```

### ⚠ bench 场景的【前置依赖】(2026-10-06 交叉验证发现 —— 以前是个假绿灯)

**依赖 `bindShapeA` 建账本的场景有 9 个**:`hot` `query` `model` `incr` `rebuild`
`prune` `iloop` `mem` `regroup`。
**不依赖的**(自带树 / 自建投影,单跑有意义):`cold` `wide` `gcstress` `deep` `dbg`。

账本只由 `scenarioCold()` 建立,而 `main` 的派发是「参数里含 `cold` 或 `pbreak` 才建账本」。
于是单跑这些场景时:

```
& bench.exe regroup  →  "旧组成片 cull -> 待回收峰值 0 组 ~ 0.0 MB"   而 exit=0   ← 假绿灯
& bench.exe mem      →  "引擎账本小计 0.0 MB" / 条目数全 0            而 exit=0   ← 假绿灯
```

⚠ **验证方(2026-10-06)先发现并修了 `mem` / `regroup` 两处**;实现方复核时实测发现
**还有 7 处同类**,其中最隐蔽的是 `prune`:

| 场景 | 单跑(账本空) | 有账本 | 差距 |
|---|---|---|---|
| rowcount-cache-hit | 100.8 ns/op | 429.7 ns/op | 4.3× |
| parentOf+rowOf | 466.8 ns/op | 1856.2 ns/op | 4.0× |
| delete-1k+revalidate | 954.6 ns/op | **50466.6 ns/op** | **53×** |
| **prune-1M** | **0.0 ns/op** | 107.0 ns/op | **根本没测到** |
| install-loop | 53.2 ms/install | 110.1 ms/install | 2.1× |

> ⚠ **上表是【加自检之前】采的历史数据**,用途只有"证明缺口存在"。
> 加自检之后,**单跑这些场景已在自检处返回,连 `[BENCH]` 测量行都不会输出**
> (验证方实测:9 个依赖场景单跑,各自只输出 1 行 `[BENCH]` = `build-1M` 建树,场景本体未执行)。
> 想复现"有账本"那一列,请用 `cold,<场景>` 或 `bench all`。

`prune` 最恶劣:账本空 → 无页可剪 → `0.0 ns/op`,**数字"看起来合理"却完全没测到东西**。
所以缺前置的判据是"**这次测量无效**",不是"这次数值异常" —— 靠看数字发现不了。

**现已修**:9 个场景开头统一加 `requireBoundShapeA()`,账本/树为空时显式判失败
(`[CHECK!] … 请用 bench all 或 bench cold,<场景>`,exit code = 1),不再静默给 0。
复核结果:9 个依赖场景单跑全部 exit=1、5 个自足场景单跑无误杀、9 种 `cold,...` 组合全通,
**漏判/误杀 = 0**。

> 🔧 **改判据时注意**:前置判据**不能**用 `hasProjection(g_tree.root)` ——
> `bindShapeA` 的 locate 只命中名字以 `A`/`P` 开头的节点,**root 自己是没有账页的**。
> 本补丁第一版就是这么写的,`bench all` 立刻红了 2 项。正确判据 = `memoryBreakdown().pages > 0`。
>
> ⚠ **回归基线随之改变**(2026-10-06):`bench mem` / `bench regroup` / `bench prune` 等
> 9 个场景**单跑的期望值从 `exit=0` 变成 `exit=1`**。这是**正确行为**(旧值才是 bug)。
> 跑回归时请用 `cold,<场景>` 组合,或直接 `bench all`。

---

## 1. 行为契约(最重要的一节 —— 每条都写清"前提"与"违反的后果")

### 1.1 API 分类表

| API | 归属 | 是否改变投影 | 是否释放 TreeGroup | 调用前提 |
|---|---|---|---|---|
| `childCount` / `child` / `parentOf` / `rowOf` | **读侧** | 可能(惰性重算) | **绝不** | 无(随时可调) |
| `isGroup` / `hasProjection` / `liveGroupCount` / `retiredCount` / `hasRetired` / `structRev` / `memoryBreakdown` | 纯观测 | 否 | 否 | 无 |
| `install` / `apply` / `refresh` / `uninstall` / `ungroup` | 写侧 | 是 | **是(立即 delete)** | **必须在 reset 括号内**(或视图未创建/已销毁) |
| `discard(dying)` | 写侧 | 是 | 否(**组进 `m_retired`**;册页立即拆) | 必须在 `delete dying` **之前**调 |
| `prune()` | 写侧 | 是(清僵尸) | **是(立即 delete)** | 必须在 reset 括号内 |
| `invalidate(p)` / `invalidateNode(n)` / `notifyRemove(p,d)` / `invalidateAll()` | **通知(v4 新增)** | 否(只 O(1) 置脏,不重算) | 否 | `notifyRemove` 必须在 `delete dying` **之前**调(它内含 `discard`);其余无 |
| `bindSpec` / `unbindSpec` / `rebind` | 写侧 | 是 | `rebind` 间接 | 必须在 reset 括号内 |
| `collectGarbage()` | **回收** | 否 | **是(唯一安全释放点)** | **`endResetModel()` 之后**;release 通道见 C3 |
| `collectGarbageSince(mark)` | **回收(细粒度)** | 否 | 是(begin 之后入队的那一段) | **`endRemoveRows()` 之后**,`mark` 取自 begin 时的 `retiredCount()` |

### 1.2 九条契约(逐条给证据与后果)

**C1 —— 读侧绝不释放展示节点。** `[实测]`
读侧四件套会触发 `revalidate`,而 `revalidate` 的淘汰分支**只摘账、不 delete**:
`dropProjection(g, deferDelete=true)` → `m_retired.push_back(g)`(均在 `TreeProjection::revalidate` 的淘汰分支内)。
- 违反后果:视图对**每个展开的组**持有 `QPersistentModelIndex`,其 `internalPointer` 就是那个
  `TreeGroup*` → 释放即 UAF,且引擎不发任何信号,视图无从得知。
- 证据:`tests.cpp` T17;`interactive_demo.cpp` Q 电池;外部探针(删光成员后只调
  `childCount(root)`,`retiredCount 0→1`、组地址未被复用)。

**C2 —— 写侧立即释放,且必须在 reset 括号内。** `[读码]`
`uninstall`/`prune` 走 `dropProjection(..., deferDelete=false)`(`TreeProjection::dropProjection` 的默认实参),
即**当场 `delete`,不进 `m_retired`**。
- ⚠ 特别注意 `prune()`:它会被 `install`/`refresh` 当"操作边界自愈"**自动执行**,所以
  **`install`/`refresh` 自身也必须处在 reset 括号内**。
- **⚠ v4 后继修订(2026-10-07):`discard` 已退出这条 —— 它改为延迟释放。**
  原因:`discard` 是 `notifyRemove` 的内含步骤,而 `notifyRemove` 的调用前提里
  **没有**"必须在括号内"(§1.1 只要求"在 `delete dying` 之前")⇒ 当场 delete 组 =
  视图的 persistent index 还悬着 = UAF。现在 `discard` 走 `deferDelete=true`:
  **册页仍立即拆掉**(它按"正在死的指针"做键,留着就是僵尸账页),**只有组对象**进 `m_retired` 等括号兑现。
  仍在立即释放的只剩 `uninstall` / `prune` —— 这两条都明文要求 reset 括号内,且 reset 通道
  作废全部坐标,所以安全口径闭合。
- 违反后果:与 C1 同形状的 UAF。

**C3 —— 回收点有两个粒度,必须匹配"坐标作废的范围"。** `[实测]`(P4 实证后修订)
定义 `TreeProjection::collectGarbage` / `collectGarbageSince`;析构兜底 `~TreeProjection`。
`NavTreeModel` 已自动履约,但**两条通道用的不是同一个粒度**:
- **reset 通道**(`endFullReset()`):`endResetModel()` 作废**全部** persistent index
  ⇒ 调 `collectGarbage()` 释放**整队列**,安全。
- **removeRows 通道**(`endRowRemoval()`):`endRemoveRows()` **只作废该 parent 下 `[first,last]` 行**
  ⇒ 只能调 `collectGarbageSince(mark)` 释放**本窗口新退役的**那一段;队列里更早入队的组
  (淘汰发生在**纯读**里、没有任何信号覆盖过它的行)**必须留着**,等下一次 reset 通道。
- ⚠ **坐标作废发生在 `end`,不是 `begin`。** 所以"在 begin/end 之内"要读成
  "**`end` 调用之后**";在 begin 与 end 之间释放是不安全的。
- **P4 实证**(`probe_review.cpp`):修复前用整队列 `collectGarbage()` 收 removeRows 窗口 →
  实测"队列 1→0(组确实被释放)"而"视图侧 `held.isValid()` 仍为 true"⇒ 那个 persistent index
  的 `internalPointer` 已悬空,之后任何 `rowCount(held)`/展开态检查都是 UAF。
  这不是"调用方违约"才有的路径 —— `notifyRemove` 的调用前提里本就没有括号要求(见 C2 修订)。
- 代价:若调用方长期只走 removeRows 通道,更早的残留会一直躺着(只是"待回收内存",不影响正确性);
  下一次 reset 会整批清掉。用 `retiredCount()` / `retiredHighWater()` 观测。
- 违反后果:**早回收** = C1 的 UAF 回归;**粒度错配** = P4 那条(同样是 UAF);
  **永不调用** = `m_retired` 堆积。

**C4 —— `m_retired` 的峰值已量化:换规则时 ≈ 账本的 12%。** `[实测]`
- 原描述"一次性 wipe 十万组峰值多吃 9~10 MB"**归因错了**,实测 `rebuild` 通道里
  `m_retired` **全程为 0** —— 因为组是被**复用**的(`matchReusable` 按 key 命中),从不 cull。
- 真正把组推进延迟队列的是**换规则**(key 全变 → 复用全失配 → 旧组成片 cull)。
- 实测(`bench regroup`,换全局 bindSpec 规则,10 万组一次性退役):
  **待回收峰值 100,000 组 ≈ 9.2 MB = 稳定账本 77.1 MB 的 11.9%**;`collectGarbage()` 耗时 **3.6 ms**。
- 取数方式:引擎用 `m_retiredHighWater`(只增不减的高水位计数)记峰值 ——
  **峰值出现在换规则那一瞬,事后采样 `retiredCount()` 只会看到 0**,这正是原缺口"未纳入基准"的真正原因。
  API:`TreeProjection::retiredHighWater()` / `resetRetiredHighWater()` /
  `memoryBreakdown().retiredHighWater`。
- 移植结论:**内存预算按"换规则/删组那一刻的组数 × sizeof(TreeGroup)"算**,
  不必为 `rebuild` 通道额外留量;再留 ~12% 余量即可。

**C5 ——(v4 重写)脏通知契约:数据变更必须通知,漏通知由卫生边界自愈。** `[实测]`
~~纪元闸门(EpochScope/checkedAt/setEpochOptimization/crossThreadEntryCount)~~ **已于 v4 整体删除**。
v4 的缓存检查只有一行:`revalidate` 开头 `if (p.valid && !p.dirty) return;`。
- **契约**:数据侧结构变更后调 `invalidate(parent)` / `notifyRemove(parent,dying)`(必须在 delete 前)/
  属性变更后调 `invalidateNode(node)` / 大变更后 `invalidateAll()`。
  通知只置脏(O(1),批量天然合并),重算发生在下次读。
- 违反后果:**不崩溃**,产生**静默陈旧投影** —— 但有三层兜底:
  ① `prune`/`rebind`/`install`/`refresh` 的 DFS 对每个账页 healIfDrift(枚举 vs lastLive,
  漂移→置脏重算,**双配置都跑**)⇒ 陈旧最多活到下一次卫生边界;
  ② paranoid 漏报检测(默认 Debug 开,`setParanoidChecks`):childCount 对干净页额外比对,
  漂移 → stderr 点名漏报 owner + 当场自愈(只对【干净】页比对,已通知的脏页不誤报);
  ③ `structRev()` 精准操作前后取值比对(C9 不变)。
- **⚠⚠ 属性漂移【没有任何兜底】(集成方必读,2026-10-07 读码核实)。**
  三层兜底比的**全部是"孩子列表"**(`healIfDrift` 拿 `rawChildren` 快照逐项比指针),而属性变更
  (材质/名字等一切影响 `filter`/`grouping`/`less` 的值)**不改变孩子列表** ⇒ 三层一层都发现不了:
  `healIfDrift` 判不出漂移、`prune` 走同一个函数也判不出、`revalidate` 因页干净直接返回。
  漏调 `invalidateNode` 的后果是**永久陈旧**(直到某次 `install`/`rebind`/reset 才被动修正),
  而且**不崩不报**。所以 `invalidateNode` 是**没有保险的硬契约**,
  与 `invalidate(parent)`(有 `healIfDrift` + paranoid 双兜底)**不是一个安全等级** —— 别并列看待。
  (要给它上保险的唯一办法:让规则产出"属性指纹"一起存进 `lastLive`,代价是每页多存一份字符串。)
- **单线程前提不变**(与 QAbstractItemModel 同级):引擎无锁;跨线程改数据树 = 调用方违约。
  ~~逃生阀 setEpochOptimization(false)~~ 随纪元机制删除 —— v4 没有"可关掉的优化",
  只有"必须遵守的通知契约"。回归:**T20(v4)** paranoid+prune 双自愈;**T22(v4)** 通知即新鲜。

**C6 —— 组不存成员;成员关系的唯一存储处是账页的 `p.members`。** `[读码]`
`TreeGroup` 只持有 `m_ticket` + `m_owner`(`class TreeGroup` 的成员区);成员靠
`freshMembers()`(`TreeProjection::freshMembers`)从 owner 账页现取。

**C7 —— `freshMembers()` 返回的是**就地引用**,只在 owner 下次重算前有效。** `[读码]`
`TreeProjection::freshMembers` 返回 `&mit->second`,不拷贝。所有调用点必须"取得即用"。
- 安全性依据(节点式容器):`unordered_map` rehash **只失效迭代器,不失效元素引用** `[论证]`。

**C8 ——(v4 作废)提示位图已整体删除。** ~~`m_pageHint`/`m_groupHint` 只置位、从不清除,置位点唯一~~
位图是 prune DFS 的微优化(省 60~80ms/百万节点),代价是 2MB 常驻 + "建页点唯一、漏置位=活页被当僵尸删"
这条易碎不变量(曾出过 M 用例事故)。v4 权衡:**边界操作省几十毫秒不值一条隐形契约** → 删除,
prune 直接查 `m_byParent`(unordered_map,O(1))。本条保留作推翻痕迹(§6.5 纪律)。

**C9 —— 失步自检:`structRev()` 的读法。** `[实测]`
递增点 `revalidate` 末尾的 `++m_structRev`(条件:`deleted` 非空 或 flat 行数变 或 组数变)。
`ITreeSource::structRev()`、`NavTreeModel::structRev()`。
- **delta = 0 不等于"形状没变"**:组蒸发常发生在**坐标查询那一步**(纯读 `rowOf`/`parentOf`
  就会当场淘汰空组),那次递增落在基线**之前**。
- 因此基线必须取在**所有坐标查询之后**;真正要保证的不变量是
  「形状变化必须被某个 begin/end 覆盖」,不是「delta 必须非零」。
- 消费范例:`gui_demo.cpp::removeWithSignal`。

### 1.3 级联预判(多层分组的正确算法)

`cascadeTarget()`(函数在 `navtreemodel.h` 末尾):沿 `parentOf` 向上连续检查 `childCount(cur)==1` 的组,
取**最外层**那个;一旦某层 `!=1` 即可 `break`(成员数只减不增)。
三个 demo(`gui_demo` / `tabs_demo` / `interactive_demo`)**共用这一份**。`[读码]`
- 为什么不能只判一层:内层组蒸发可能连锁让外层组也空,只发一条信号 ⇒ 模型/视图失步。
- **`[实测]` 已有专项回归 `unit_tests` T19**:真造出 `root → Outer → Inner → Only` 四层,
  删最内层唯一成员 → 两层一起蒸发、**只发一条信号**、`rowCount` 恰好 -1、无僵尸行;
  并带**分辨力自检**(断言单层判法会给出 Inner 而非 Outer)⇒ 该断言非恒绿。

### 1.4 造多层嵌套的正确姿势(实现方踩坑记录,供移植参考)`[实测]`

分组规则作用在 `parts = vis + 旧组` 上 —— **组能被收进桶靠的是"旧组参与 parts",
而不是 `rawChildren` 里出现组**(组的成员表里只有数据节点)。所以必须两步:

1. 第一次 `install` 让数据节点成组(`Inner`);
2. 第二次 `install`:此时 `Inner` 作为旧组参与 `parts`,被 rule 收进另一个桶(`Outer`)。

- ⚠ rule **必须只收 `key == "Inner"` 的组**,否则 `Outer` 会把自己也收进去 → 无限套娃。
  这正是工厂 `GroupRules::byKey` / `byPredicate` **无条件豁免组节点**的原因(手写规则绕过了该豁免,须自行承担)。
- ⚠ 定位内层节点要用 `parentOf(数据节点)` **逐层上问**,**不能按名字扫一层**
  (`Inner` 已在 `Outer` 内,扫一层必然返回 nullptr)。实现方在此处连踩两次(Q 电池一次、T19 一次),
  且两次都因**静默 `return`** 导致后半段测试根本没执行却显示"0 FAIL"。
  → **教训:测试中途 return 必须伴随显式 `check(false, …)`。**

### 1.8 编译警告纪律(`/W3` 落地)`[实测]`

CMakeLists 统一 ` /utf-8 /W3`,**故意不加 `/WX`**(先看得见再逐条收拾)。
clean build 双配置结果:**0 错误 0 警告**。

**⚠ 一条硬规矩:printf 格式串里只能用 ASCII 符号。**

`→ ★ ─ │ ≈ ⇒` 这些字符 GBK 表示不了,会破坏 MSVC 的 printf 格式串分析(字符串被截断错位)
→ **C4474「格式串中指定的参数太多」**。⚠ **代价是真实的**:屏蔽 C4474 = printf 参数
类型/数量检查失效,以后真写错参数也不会报 —— 所以**改格式串,而不是加 `/wd4474`**。
(本轮为此把 bench.cpp 的 `WS %.0f→%.0f` 改成 `%.0f->%.0f` 等 16 处。)

**已豁免的 4 个编号(逐条附理由,见 CMakeLists 注释)**:

| 编号 | 理由 |
|---|---|
| `/wd4127` | 条件表达式恒定 —— Qt 的 `Q_UNUSED`/`Q_ASSERT` 与 constexpr 判断必然触发 |
| `/wd4514` | 未被引用的 inline 被移除 —— 库头常见,不占代码空间 |
| `/wd4668` | 预处理器宏未定义 —— Qt 的 `Q_OS_*`/`Q_CC_*` 探测宏 |
| `/wd4819` | **已实证是误报**,调查链见下 |

**C4819 误报的完整调查链**(留着免得有人重新怀疑一遍):
1. `/utf-8` 确实在 `cl` 命令行里(不是配置没生效);
2. 全部源文件是**合法 UTF-8、无 BOM**(用严格 `UTF8Encoding` 逐字节校验);
3. 加 UTF-8 BOM **无效**(警告数不变)→ 已回退,不留下无意义的文件改动;
4. 最小复现:纯中文 + `/utf-8` **不报** C4819;
5. 真正的触发源 = 源码里的**装饰性 Unicode 符号**(制表线、箭头、`★`)。

所以剩下的 28 处 C4819 **全部只在输出文本/注释里**,不影响任何逻辑 → 豁免合理。
⚠ 但新增源文件仍须保证**合法 UTF-8**(编译期由 `/utf-8` 保证,运行时输出由 `CP_UTF8` 保证)。

### 1.7 ~~纪元优化的逃生阀~~(v4 已删除整节机制,保留标题作推翻痕迹)

> **v4 注**:纪元机制(EpochScope/checkedAt/开关/跨线程检测)已整体退役,
> 替代物是 C5 的脏通知契约 + `setParanoidChecks(bool)`(漏报检测,非性能开关)。
> 以下为历史原文,仅供理解旧版本:

前提不成立(你们的模型跨线程/异步改数据树)时,现在有**正式的运行时开关**,不用改源码:

```cpp
TreeProjection::instance().setEpochOptimization(false);   // 退回"每次都重新枚举活列表"
```

- **行为完全不变,只是慢一点** —— 它关掉的只是"同一次操作内同一账页只校验一次"这个短路;
  惰性发现数据变更的能力**一点没少**(那靠的是活列表枚举,不是纪元)。
- 关闭时引擎会**清零所有页的 `checkedAt`**,让"关掉就一定全量枚举"成为可推理的不变量。
- **不是性能开关**:别为提速去关。
- 配套诊断:`crossThreadEntryCount()` 返回"检测到过的跨线程进入次数"(正常恒 0)。
  ⚠ **Debug 下才真正比对线程身份**(Release 只累加计数),所以 Release 的 0 **不是**"没问题"的证据。
  Debug 下检测到跨线程会往 stderr 报一次并提示调本开关。

**回归**:`unit_tests` **T20** 验证三件事 —— ①开关真接在闸门上 ②**开/关两种状态下投影树逐项相同**
(这才是"逃生阀"的定义)③关闭状态下数据侧删除仍被惰性发现(组自动蒸发)。实测两配置全绿:

```
[OK] ① 默认:纪元优化是开着的
[OK] ① 数据未变时重复查询不改变结构(闸门语义正确)
[OK] ② 开关开/关,投影树逐项相同(逃生阀只改快慢,不改行为)
[OK] ★② 关闭状态下,数据侧删除仍被惰性发现(组自动蒸发)
```

### 1.5 异常安全

`valid` 在**流水线全部跑完之后**才置位(`revalidate` 末尾的"宣布有效"段),此前任何
用户回调(`filter`/`grouping`/`less`)抛异常都会让本页停在 `valid=false` + `dirty=true` → 下次重查自动收敛。`[读码]`
- 注意:`lastLive` 是在流水线**之前** swap 的,所以抛异常后本页会重跑整条流水线(这是期望行为)。
- ⚠ **异常会留下一个"半新半旧"的页(2026-10-07 读码发现,未修)**:`page.members` 在流水线中段
  就 `std::move` 换新,而 `page.ownedGroups` 要到末尾才更新,中间还夹着 `sortMembers`/`sortFlat`
  两次调用用户 `less`。`less` 若抛异常 ⇒ 这一页停在"members 是新的、groups 是旧的";
  下一轮 `oldGroups = page.ownedGroups ∪ members 键` 就可能装进**两个 key 相同的组**
  (aborted 那轮新建的 + 旧的),`matchReusable` 的 byKey 只能认领一个,另一个落 culled。
  **这正是 `revalidate` 里 `groupsKeptAsMember` 白名单必须保留的原因**(它拦的就是这条路径上的 UAF,
  而不是"多层分组的存活路径"—— 那个理由在 Stage B-2b 闭包落地后已不成立)。分级:低概率 + 需要用户 `less` 抛异常,故未修。

### 1.6 ★验证方法的边界:`QAbstractItemModelTester` 查不出"信号发错行" `[实测]`

⚠ **首版基线里那句"Tester 正好覆盖'读查询改结构 / 信号发错行'"是错的,已推翻。**
本节是**验证方/实现方都极易踩的坑**,独立于引擎代码本身,故单列。

**为什么查不出**:Tester 检查 `index/parent/rowCount/data` 的互逆性与信号配对。但模型的
`rowCount()`/`index()` 每次都**重新问引擎拿当前真实结构**,所以"模型报告的结构"永远自洽;
而"发出的信号与实际结构变化是否匹配"完全不在它检查范围内。

**注入实验铁证**:故意把组蒸发发成组内成员行删除(诊断打印 `isGroup(par)=1, childCount=1`
—— 组必蒸发),Tester 仍报 `全程无断言 = true`。

⚠ **同源的第二个陷阱(实现方第一版就踩了)**:**别用"删除前后 `rowCount` 差值"验证信号** ——
它每次重问引擎,差值恒等于真实减少量,与信号发对发错**毫无关系**。实测反例照样"通过"。

**唯一有效的做法**(`tests.cpp` T18② 现在这么做):捕获 `QAbstractItemModel::rowsAboutToBeRemoved`
的**参数本身**,断言 `(parent, first, last)` 等于 `cascadeTarget` 算出的组行坐标。

⚠ 反例对照必须比 **`parent`** 而**不是 `row` 数值** —— 实测该场景下"组内成员行号"与
"组行号"**恰好都是 0**,比数值会漏判。

---

## 2. 不可破坏的不变量(改动自检清单)

任何改动提交前,逐条自问:

1. **读侧不得 `delete` 任何 `TreeGroup`。** 唯一的 `delete` 只在 `collectGarbage()`
   与 `dropProjection` 的 `deferDelete=false` 分支(`else delete g;`)。
2. **`revalidate` 内只准用 `rawChildren`**,不得调公共 `childCount`(否则"对组分组"无限递归)。
3. **绑定谓词不对组节点求值**(`if (!isGroup(n) && b.locate(n))`),否则无限嵌套。
4. **落位时必须剔除本轮已删的旧组**(`deleted` 集合),否则野指针进 `flat`。
5. ~~**建页必置位**(提示位图)~~ **v4 作废**:位图已删除,建页无附加义务(仍走 `projection()`)。
   **v4 新增:数据变更必配对通知** —— 一切绕过引擎的数据侧增删都要有
   `invalidate`/`notifyRemove`/`invalidateNode` 配对(delete 之前调);
   收口点模式见 interactive_demo 的 `createAndAttach`/`deleteChildRaw` 包装。
6. **`m_retired` 的清理只在 `collectGarbage()`,且只在 begin/end 之内。**
7. ~~**`memberToGroup` 只存本层**(刻意无全局版,多层会互相覆盖)~~ **v4 作废**:该表已并入每页单表
   `entries`,随页同生共死。**替代不变量**:"成员→坐标"只存在于每页的 `entries`,进程内**没有全局副本**
   (全局反查表在多层下会互相覆盖),`parentOf` 靠沿自然父亲逐层下钻,天然 deepest-wins。
8. **投影引擎是进程级单例**(`TreeProjection::instance()`),多文档共享一套 keyspace;
   跨根定位必须靠谓词自限定(如 `topOf(n)==root`)。
9. **测试中途 `return` 必须伴随显式 `check(false, …)`** `[实测]`。静默 return 会让后半段
   根本没执行却显示"0 FAIL" —— 实现方两次自踩(Q 电池、T19)。
10. **场景/基准必须有"前置依赖自检":依赖的投影或账本不存在时,必须 `checkOk(false, …)`,
   绝不静默跑成一堆 0** `[实测]`。理由:`bench regroup`/`mem` 单跑时账本为空 → 读数全 0
   而 exit=0,看起来"通过" —— 与第 9 条同源,都是**把"没测到"伪装成"测过了"**。
   参考实现:`bench.cpp::requireBoundShapeA()`。
11. **成员表必须闭合:`page.members` 每个 value 里出现的【组】,必须也是这张表的 key。**
   `[实测]`(2026-10-07 落地)违反 = **空壳组**:视图上多一个零成员的空组行,而且更糟 ——
   空壳只是 members 的 value、不是 key ⇒ 不在 `page.ownedGroups` 里 ⇒ `dropProjection`/`uninstall`
   遍历不到它、`liveGroupCount()` 也数不到它 ⇒ **只在全局 `m_groupOwner` 里留一条静默泄漏**。
   维持者:`Pipeline::closeMembership`(Stage B-2b,迭代到不动点:剔除"自己的 key 没有桶"的组
   + 丢掉随之变空的桶;轮数 = 嵌套深度)。**判据/回归**:`memoryBreakdown().groupOwner == liveGroupCount()`
   —— 空壳会打破这个等式,而 `liveGroupCount()` 单独看是盲的(`unit_tests` **T24**)。

---

## 3. 性能与内存基线

> ⚠ **本节所有绝对读数都出自 `bench full`(原规模 1,010,101 节点)。**
> 2026-10-07 release 分支起,`bench` **默认跑快档**(形状完全不变、规模缩小到 5,111 节点
> / 110 页),目的是让整套回归的墙钟从 Release 58 s / Debug 321 s 降到 **Release 1.7 s /
> Debug 7.7 s(七目标合计)**;快档只打印按比例缩小的账本条目数,**不要拿快档的输出对本文的表**。
> 换档命令:`bench full`(全场景原规模)/ `bench full,cold,mem`(原规模 + 单场景)。
> 断言纪律随之收紧:**bench 里不许写死节点数**,一律用 `g_scale` 派生量 ——
> 两档共用同一套 CHECK,写死即换档变红。

> 机器:验证方本机(Release)。**换机器请重新采基线,不要把绝对值当回归阈值**,
> 而应看同一台机上的相对变化。

| 场景 | v3(评审轮后) | **v4(脏通知,2026-10-07)** | 说明 |
|---|---|---|---|
| `build-1M`(数据侧建树,引擎无关) | ~112 ms | ~156 ms | 机器噪声量级 |
| `cold-bind`(bindSpec+rebind 装 1 万父) | ~938 ms | **~626 ms** | 含全树 DFS + prune |
| `rowcount-cache-hit` | ~394 ns/op | **~62 ns/op** | ★6.3×:干净页 = 一次哈希 + 脏标志,零枚举 |
| `parentOf+rowOf` | ~1342 ns/op | **~867 ns/op** | locate 下钻 O(深度) 哈希(随机访问 1M 树,含 cache miss;v3 中期 959 那个数字靠不健全跳过换来,已作废) |
| `model-full-dfs`(111 万 index) | ~925 ms | **~359 ms** | ★2.6× |
| `rebuild-channel` | ~1750 ms | **~1123 ms** | wipe 95 + rebuild 65 + prune冷 684 + walk 279 |
| `prune-1M` | ~113 ms | **~202 ms** | ⚠ 变贵是【故意的】:prune 兼职"漏通知自愈"(每页 healIfDrift 枚举比对)—— 卫生边界买正确性兜底 |
| `wide-rowOf`(20 万孩子父) | ~230 µs/次 | **~75 ns/次** | ★3000×:entries 直接答行号 |
| `wide-install-200k` | ~710 ms | **~300 ms** | |
| `install-loop`(反例) | ~576 ms/install | ~204 ms/install | 仍是 N×install 禁忌的实证(每次内嵌全树 prune) |
| 引擎账本 | **228.6 MB** | **108.6 MB** | ★省 52%:三表(memberToGroup 54.1 + memberToPos 74.3 + m_nodePage 65.9)→ 单表 entries 74.3;位图 2MB 也删了 |
| 峰值工作集 | ~489 MB | **~316 MB** | 数据 166 + 账本 108.6 + 组本体 ~9 + 碎片 |
| 跨调用 O(k²) | **未消除** | **根除** | T21:600 宽页全树 parentOf+rowOf,Debug **13 s → 1.06 ms**;根治手段正是数据侧信号(C5 v4 通知契约) |
| `gcstress` | ✓ | ✓ WS +1.1 MB | 200 轮双路径,Debug `_CrtCheckMemory` 0 失败 |

### 读侧位置索引(2026-10-06 落地):拿到了什么、没拿到什么

> **⚠ v4 注(历史小节)**:本节的 `memberToPos`+`m_nodePage` 双表已被 v4 的**单表 `entries`**
> (每页 `node→{投影父亲,行号}`,无全局副本)取代;"跨调用 O(k²) 未消除"的诚实结论已被
> **脏通知契约**(C5 v4)解决 —— 干净页零枚举,行号/父亲直接查表。下文保留作演化痕迹。

**问题**(实测):模型对每一行都问 `parentOf`/`rowOf`,旧实现为给答案必须
`revalidate` → `liveMatchesLast` **枚举该页全部孩子**。而**每次模型调用都是一个独立操作
(各自开纪元)**,所以"页级校验一次"跨调用无法复用 ⇒ 遍历 k 行 = k 次 O(k) ⇒ **O(k²)**。

**改法**:每页新增 `memberToPos[节点] = (投影父亲, 行号)`,全局新增 `m_nodePage[节点] = 所属页`;
`parentOf`/`rowOf` 先按页校验一次、再从索引取答案。`dropProjection` 按本页键集点查清理。

**✅ 拿到了什么**(用计数探针证明,见 `probe36`):
- `parentOf` 的**答案**不再需要"逐层下钻 + 全页扫描":索引里存的就是已解析好的投影父亲,
  以及**正确的行号**(★ 关键坑:行号不能用自然父亲 `flat` 的位置 —— 被分组的成员根本不在
  自然父亲的 `flat` 里(那里放的是组),它的行号是"在所在组的成员序列里的位置")。
- **操作内部的多次查询**显著变快:同一操作内对同一页的重复校验被压成一次。
- 改造前 `parentOf` 是 O(k) 纯扫描;索引把"查表"部分降到 O(1)。

**❌ 没拿到什么(必须知情,不要被"O(1)"的说法误导)**:
**跨调用的 O(k²) 没有消除。** 实测(独立理想数据层探针):

| 最宽层 k | 一次全树遍历(每行问 parentOf+rowOf) | 每行耗时 |
|---|---|---|
| 500 | 0.35 ms | 0.69 µs |
| 2000 | 5.16 ms | 2.58 µs |
| 5000 | 32.4 ms | 6.47 µs |
| 10000 | 121 ms | 12.1 µs |
| 20000 | 485 ms | 24.2 µs |

仍然是**超线性**(每行耗时随 k 增长),因为每一次模型查询都重开纪元 ⇒ 页校验无法跨调用跳过。
计数探针给出确凿证据:`N=10000` 的一次遍历中,数据侧枚举接口被调用 **100,050,002** 次
= 每行 ~10002 次(= k)。

**要真正消除它,只有一条路**:让数据侧给一个**变更信号**(脏标记/版本号/写侧 API 统一入口),
使引擎不必靠"枚举活列表"来发现变化。这属于数据层契约变更,不在本次改动范围内。

**为什么不再往前一步**:曾尝试"跨操作复用成员表指针 + 用页的 `checkedAt` 做钥匙"两次,
两次都引入了**难以复现的陈旧读**(`T16 事前坐标删除协议` 在 Debug 稳定失败、Release 通过),
根因是缓存持有的 `vector*` 与被 `clear()` 掉的 map 节点生命周期无法可靠绑定。
按"正确性优先"整段撤除。伪代码层面的教训:**在"惰性发现变更"的引擎里,任何"跳过校验"的
缓存都必须有来自数据侧的可靠失效信号,否则迟早变成 UBSAN/Heisenbug。**

**实测内存代价**(`bench mem`,百万被投影节点):

| 项 | 改造前 | 改造后 |
|---|---|---|
| `memberToPos` | — | **74.3 MB**(111 万条目) |
| `m_nodePage` | — | **65.9 MB** |
| 引擎账本小计 | 88.4 MB | **228.6 MB** |
| 峰值工作集 | ~299 MB | **~496 MB** |

**回归护栏**:`unit_tests` 新增 **T21**(宽节点读侧),断言宽节点全树 `parentOf/rowOf` 一遍
不得退化成秒级;它同时校验全树不变量,防止"索引化改错答案"。`bench` 侧 `parentOf+rowOf`
从 2323 → 959 ns/op(k=100 的窄树,绝对收益有限)。

> ⚠ 本轮改动**只动了测试代码与文档**,未触碰引擎热路径,故以上数值与首版基线同量级;
> 差异来自机器噪声。若验证方复跑得到显著差异,先排除 CPU 频率/后台负载。

### 生产纪律(已被数据坐实,不是建议)
1. **批量装配走 `bindSpec` + `rebind`,绝不 N×`install`。**
   `install` 内嵌一次全树 `prune`:单个 install 实测 **119.7 ms**,1 万父逐个 ≈ **20 分钟**;
   走绑定一次 **0.5 s**。
2. **`prune`/`rebind` 是全树 DFS(~120 ms/百万节点)**,别放进每帧或每行绘制路径。
3. **超深链:引擎 DFS 已全部显式栈迭代化**(`bench deep` 10 万深链 PASS),但
   **`~GuiBase` 的连坐析构仍是递归的** —— 深度 >3 万级时 `delete root` 本身会爆栈。
   这是冻结架构的属性,改引擎解决不了。
4. **单线程约束 + 通知契约**:见 C5(v4)。数据变更必须配对通知;跨线程改数据树 = 违约(引擎无锁,与 QAbstractItemModel 同级假设)。
   ⚠ 其中 **`invalidateNode`(属性变更)没有任何兜底** —— 见 C5 里那条警告,别按"兜底能救"来接。
5. **内网首发把 `setParanoidChecks(true)` 打开(Release 也开)。** `[建议,待内网数据回填]`
   v4 的失败模式是**静默陈旧**(漏通知 → 显示错树,不崩不报),而 paranoid 是
   **Release 下唯一能发现漏通知的手段**(三层兜底里的 ① 只在卫生边界生效,③ 要调用方主动用)。
   代价 = 每次 `rowCount` 在干净页上多一次枚举;而导航树的真实访问模式是"**每个父亲问一次**",
   不是"同一页反复问",所以代价 ≈ **一次额外枚举/展开**,不是 O(k²)
   (§3 表里 `rowcount-cache-hit` 的 6.3× 差距是"同一页反复问"的极端场景,不代表界面行为)。
   计划:首发开着 → 稳定后按 stderr 点名出的"漏通知清单"逐处补通知 → 再关。

---

## 4. 测试与回归基线

| 目标 | 期望 | 实测(Release / Debug) |
|---|---|---|
| `unit_tests` | 退出码 = 失败块数 = 0 | **27 块全绿,双配置**(2026-10-07:新增 **T24 空壳组** —— 用"归属表条目数 == 活组数"这个恒等式抓出"活着却不归任何页管"的静默泄漏,修复前 3 条 FAIL 作分辨力;新增 **T25 closeMembership 纯函数** —— 四类边界,其中"多层依赖"是为"把迭代改成单轮过滤"准备的红靶子;此前 2026-10-06 评审轮新增 T22 裸坐标新鲜度 / T23 entries 随页清理;T21 阈值分配置,见 §7 评审记录);`live groups at exit = 0`。⚠ Debug 版已静音 CRT 弹框,跑测试不应再弹窗 |
| `navtree_demo` | 退出码 0,不变量通过 | ✔ 通过 |
| `gui_demo` | 退出码 0 | ✔ 模型不变量 13 项 + 终检 10 项 |
| `tree_tabs --selftest` | `selftest PASS` | ✔ |
| `interactive_demo --selftest` | `selftest PASS` | ✔ **62 条 [OK] 全绿**(A~S;v4 迁移后,Q 电池改走通知契约,断言主体不变) |
| `bench`(默认=快档) | `CHECK` 全 OK、teardown 零泄漏 | ✔ CHECK 失败=0,**墙钟 < 1 s**(2026-10-07 起;改档前 Release 58 s / Debug 321 s) |
| `bench full`(原规模) | `CHECK` 全 OK | ✔ CHECK 失败=0,**墙钟 ~57 s**;§3 表里的绝对读数全部出自这一档(账本 108.6 MB / 100000 组 / 9.2 MB 峰值 —— 已逐项复现) |
| `bench deep` | PASS(无栈溢出) | ✔ depth 100000 |
| `bench dbg` | 无 MISSING | ✔(注意:其"address reuse"统计的是**叶子**地址复用,与组回收无关) |
| `bench full,cold,regroup` | CHECK 全 OK | ✔ 换规则 ~742 ms / 待回收峰值 **100000 组 ≈ 9.2 MB** / 新存活 10100 组 / 规则可复原。⚠ 这组数字是**原规模**的,必须带 `full`;快档下按比例缩小(1000 组 ≈ 0.1 MB)。**单跑 `bench regroup` 故意 exit=1**(缺前置,见 §0) |
| `bench gcstress` | 残留=0、无堆损坏、无累积泄漏 | ✔ Debug:200 轮(100 轮走 prune 野 key 路径)堆校验 **0 失败**;Release:`[SKIP]` 堆校验、WS +3.0 MB。详见 §5「ASan 替代实证」 |
| `bench cold,<场景>` × 9 | exit=0 | ✔ 依赖账本的 9 个场景(`hot/query/model/incr/rebuild/prune/iloop/mem/regroup`)必须带 `cold` 前缀 |
| `bench <场景>` × 9(单跑) | **故意 exit=1** | ✔ 缺前置 = 测量无效,显式判失败(旧行为是静默给 0 + exit 0,那才是 bug) |
| `probe_review` | `summary: 0 confirmed` | ✔ 四个探针全 OK:P1 通知契约两半 / P2 entries 随页 / P3 组上零求值(V4 前身 R1/R2/R3,2026-10-06 评审轮)+ **P4 退役组 vs 视图 persistent index**(2026-10-07)。P4 两段判据:**P4a** removeRows 窗口不得释放"未被本次信号覆盖"的更早残留;**P4b** reset 通道必须"既释放(retired=0)、又已作废坐标(held.isValid=0)"。**修复前 P4 CONFIRMED**(实测 retired 1→0 而 held 仍有效),修复后两段皆 OK |

### 行覆盖率(2026-10-07 release 分支新增)`[实测]`

**命令(可复跑,零额外依赖):**

```bash
python tools/coverage/run_coverage.py      # 退出码 0 = 测试全绿 且 覆盖率 ≥ 85%
```

**实测结果:引擎行覆盖率 94.3%**(可执行行 796/844)。

| 文件 | 覆盖 / 可执行 | |
|---|---|---|
| `treeprojection.cpp` | 517 / 555 | 93.2% |
| `treeprojection.h` | 39 / 41 | 95.1% |
| `projection_stages.h` | 113 / 116 | 97.4% |
| `ptrset.h` | 68 / 70 | 97.1% |
| `guibase.h` | 59 / 61 | 96.7% |
| `itreesource.h` | 0 / 1 | 纯虚析构,不可调用(不是缺口) |
| **小计** | **796 / 844** | **94.3%** |
| `navtreemodel.h` | — | **本机不可插桩**(需真实 Qt 运行库);其行为由 MSVC 侧 T15/T16/T18/T19 覆盖 |
| `demonodes.h`(测试夹具,不计入) | 42 / 50 | 84.0% |

**为什么是 MinGW g++ 而不是 MSVC**:本机 VS2019 只装了 `clang-format`/`clang-tidy`
(**没有** clang-cl),也没有 OpenCppCoverage,而 `cl.exe` 本身不给行覆盖率;
Qt 自带的 MinGW g++ 13.1 + gcov 是完整可用的 —— 且**引擎与 Qt 运行库无关**,
所以能把 `tests.cpp` 编成 `NT_ENGINE_ONLY` 变体做真测量。脚本里有两个必须做对的机关:
① **假 `QObject`**(`tools/coverage/QObject` 靠 `-I` 顺序遮蔽 `<QObject>`):`guibase.h` 里
`GeomObject` 继承 `QObject`,而引擎不碰 Qt 实现 → 用替身即可编译三政体代码,**交付代码一行不改**;
② **跨 TU 合并**:pipeline 的 inline 函数会被链接器 COMDAT 去重只留一份,单看某个 `.gcno`
会得到"0 行执行"的假象 → 脚本把两个 TU 的 gcov 结果**逐行取并集**。

**剩余 48 行的性质(已逐条核对,不是"不知道"):**
- **退出期测量假象(12 行)**:`~TreeProjection` 的整个函数体 —— 它在进程退出时执行,
  而 MinGW 的 gcov flush 排在 atexit 链之前,计数写不进去(该路径**每次都跑到**了,
  只是量不到;试过 `-fno-use-cxa-atexit` 无效,已撤)。
- **按设计不可达(5 行)**:`revalidate` 里 `groupsKeptAsMember` 白名单(同 key 重复旧组那条
  病态路径)、`parts` 拼装末尾的兜底 while —— 两者都在源码注释里写明"结构上不可达"。
- **防御性分支(约 24 行)**:`PtrSet` 成环守卫、`collectReachable` 的 `continue`、
  `locate`/`rowOf` 的"被过滤/页已拆"回退等 —— 需要构造病态数据才能进,收益低于成本。
- **1 行**:`ITreeSource` 的纯虚析构。

**覆盖率工作直接换来的两个真实修复**(不是为了让数字好看):
1. **`PtrSet::reserve` 静默丢数据**(`ptrset.h`):旧实现 `m_table.assign(capacity, nullptr)`
   把整张表换成空槽却**不重插已有元素**,`m_count` 还与表内容脱钩 —— 因为唯一调用点是
   构造函数的"作用于空表"那一处,所以一直没爆。T27 抓到并修。
2. **测试电池从未碰过的接口面**:`apply` / `refresh` / `install(nullptr)` /
   静态 `pool`(siblings)/ `invalidateNode` / `invalidateAll` / `collectGarbageSince` /
   宽亲(>256)槽位表 / 塌缩清桶 / `healIfDrift` 的早退与**等长内容漂移** ——
   现在由 **T26** 与 **T27** 覆盖(见 §4 表)。

### bench 的 CHECK 退出码契约(2026-10-05 新增,踩坑换来的)

**`bench` 的退出码 = CHECK 失败数**,不再是恒 0。

⚠ **为什么这条值得单列**:原先 11 处 `[CHECK] … OK/MISMATCH` 全是裸 `printf`,
`MISMATCH` 只打在屏幕上、exit code 照样 0 —— **自动化回归完全抓不到**。
这比没有检查更危险:它让人以为检查过了。实际后果:我看到
`gcstress` 报 `MISMATCH` 而 `bench all exit=0`,却差点当成"噪声"放过。

改为统一走 `checkOk(bool, fmt, ...)`:失败时计数 +1、行首打成 `[CHECK!]`,
`main` 返回 `g_checkFail ? 1 : 0`(`all`/`deep`/`dbg` 三个入口都改了)。

**这个机制上线后立刻抓出一个被掩盖的真实 bug**:`bench dbg` 场景
`unregisterRoot` 后直接 `prune()`,而 **`m_roots` 为空时 `prune` 直接 return(设计如此)**
→ 僵尸账页一个没清 → 残留组。`main` 场景一直守着"清僵尸要挂临时探针根"这条规矩,
`dbg` 场景漏了,又被 `return 0` 掩盖。已修(挂 `__purge_dbg__` 探针根)。

⚠ **移植时的对应纪律**:任何"检查类输出"都必须能反映到退出码,否则 CI 是假的。

### 1.9 gcstress 的局部/全局计数纪律

`liveGroupCount()` 是**全引擎**计数。断言"本场景结束时组数为 0"在串跑时必然误报
(`bench all` 里 gcstress 排在 regroup 之后,主树还有 10 万组 → 报"残留 20000000")。
正确做法:进场景时记**基线**,断言"结束时回到基线",而不是断言绝对 0。

### 必须长期保留的回归项(用户现场钉死的)

- `interactive_demo`:**L** 同名组无限嵌套(用户原崩溃)、**M** 重建后重复节点(点一选俩)、
  **N/O** 多层后深层再建组、**P** 三层嵌套收组失败、**Q** 读侧不释放组。
- `unit_tests`:**T17** 读侧不释放组契约、**T18** Tester(查账本自洽)+ `rowsAboutToBeRemoved`
  参数断言(查信号坐标,**Tester 自己查不了这个**)、**T19** 多层级联(含分辨力自检)、
  **T20(v4)** 漏通知检测与自愈(paranoid 开=childCount 当场自愈;关=陈旧至 prune 边界)、
  **T21** 宽节点读侧(v4:双配置毫秒级,阈值 Release 2s/Debug 30s 只防真爆炸)、
  **T22(v4)** 通知即新鲜(notifyRemove 后裸 rowOf 无需预热)、**T23(v4)** entries 随页同生共死。
- `interactive_demo`:**R 电池(压力树)** + **S 电池(控制台)** —— 会话内自建一棵极端复杂树
  (宽 500 直属孩子 / 24 层深链 / 三政体 / 三个同名"轴承" / 空组)并断言:535+ 节点的
  **全树不变量**、无 flat 重复、深链 24 层、宽父亲收敛成 10×50、多根隔离、同名节点各归本层、
  根页手动组与子页材料规则共存、重建后再装、拆树收场干净。
  **手工把玩入口**:①顶部控制台一行按钮(构造/拆除压力树、规则·按材料/按类型/清除、
  建组:两库合一/深链条目、展开·折叠全部);②**选中任意节点 → 状态板实时显示它的
  投影坐标**(投影父亲/投影行号/投影孩子 vs 原始孩子/自然父亲)—— 这是"数据树与投影树
  分叉了多远"最直观的一屏;③右键菜单的删除/新增/分组/删除组/重建。
  S 电池专门无头驱动控制台那三个"规则"按钮(平时只能靠鼠标点到),防止"只有手动点才会崩"的路径漏检。
  **启动即进压力树**(不再需要先点按钮);`--smoke` 无头打印"界面此刻该显示什么"
  (窗口标题 / 模型 root / 根层行数 / 存活组 / 状态板首行 / 按钮数,并点一次"规则·按类型"验证按钮真接在引擎上),供无人值守与"我这版 exe 到底显示的是哪棵树"的排查。
- ⚠ **回归项的"分辨力"要能被证明**:一个恒绿的断言等于没测。T18/T19 都刻意带了
  "反例对照/分辨力自检" —— 证明单层判法、成员行坐标会给出**不同**答案。
  新增回归项时请照此办理。

---

## 5. 已知缺口与待办(按优先级)

| # | 事项 | 状态 |
|---|---|---|
| 1 | ~~`tests.cpp` 里 `check(P.hasProjection(w.root) \|\| true, …)` 是恒真断言~~ | **✔ 已修(2026-10-05)**。它掩盖了一个真实事实:**root 本身没装投影**(本块只给 `folder` 装)。换成两条有判别力的断言:①`prune` 不凭空造账页 ②活着的 5 个节点一个不少且死者缺席 |
| 2 | ~~C5 的单线程前提只有注释,无断言、无开关~~ | **✔ 已关闭(2026-10-05)** `[实测]`。见 §1.7:新增 `setEpochOptimization(bool)` 逃生阀 + `crossThreadEntryCount()` 诊断 + Debug 下跨线程检测;`unit_tests` **T20** 验证"开/关行为逐项相同"。**注意:这是设计约束的显式化与兜底,不是把约束消除了** —— 跨线程改数据树仍应关掉优化 |
| 3 | ~~`m_retired` 峰值未纳入内存基准(C4)~~ | **✔ 已关闭(2026-10-05)** `[实测]`。见 C4:换规则时 10 万组退役、峰值 **9.2 MB ≈ 账本的 11.9%**、回收 3.6 ms;新增 `bench regroup` 场景 + 引擎高水位计数。**同时修正了原描述的错误归因**(不是 wipe-rebuild,是换规则) |
| 4 | ~~警告等级 `/W1`~~ | **✔ 已关闭(2026-10-05)**。CMakeLists 统一 `/utf-8 /W3`,噪声按编号精确豁免(`/wd4127 /wd4514 /wd4668 /wd4819`,每条附理由),**不加 `/WX`**。clean build 双配置 **0 错误 0 警告**。详见 §1.8 |
| 5 | `.gitignore` 缺失;`build/` 与源码混在同一棵树 | ⚠ **本项作废**:本仓库**不是 git 仓库**(实测 `fatal: not a git repository`),`.gitignore` 无意义。`build/` 体积见 §5 |
| 6 | ~~`bench dbg` 的地址复用检查只覆盖叶子~~ | **✔ 已覆盖(2026-10-05)**:组地址复用现由 **`unit_tests` T17** 与 **`interactive_demo` Q 电池**覆盖(删光成员→组进 `m_retired`→重生组地址**实测不同**)。`bench dbg` 那条统计仍只测叶子,保留作叶子侧的对照 |

### 明确"未实证"的事项(不要当成已验证)

- **`collectReachable` / `prune` 与"账页 key 已被数据侧释放"的内存安全**。
  ~~`[论证]`(ASan 起不来)~~ → **`[实测]`,见下方"ASan 替代实证"**。
- ~~`test` 侧对**信号参数**的校验~~ **→ 已解决 `[实测]`**:T18② 现在捕获
  `rowsAboutToBeRowsRemoved` 并断言 `(parent, first, last)`;
  T19 断言两层级联"恰好 1 条信号 + 坐标正确 + 无僵尸行"。
  ⚠ 但**方法本身的坑**见 §1.6 —— 这正是首版基线写错的那条。

### ASan 替代实证(把 prune 内存安全从 `[论证]` 升到 `[实测]`) `[实测]`

本机 MSVC AddressSanitizer 运行时无法初始化(进程起不来),所以用**两个互补的证据**替代:

1. **Debug CRT 堆校验** —— `_CrtCheckMemory()` 校验**整个 CRT 堆**,
   写野指针 / 双重释放 / 越界都会被抓出来(ASan 能抓的那一类)。
   ⚠ 只存在于 **Debug CRT**;Release 下 `_CrtCheckMemory` 不可用,
   场景会显式打印 `[SKIP]` 而**不是** `OK` —— 避免假绿灯。
2. **泄漏曲线** —— Release 反复建组/删成员/回收 N 轮,看工作集是否单调增长。

**场景**:`bench gcstress`,200 轮(每轮 200 节点)。**两条删除路径轮换**,这是关键:

| 路径 | 轮数 | 做法 | 验证什么 |
|---|---|---|---|
| A | 100 | 删前 `discard()` 显式讣告 | 账页**当场清**、组进 `m_retired`(2026-10-07 起延迟,见 C2 修订) |
| B | **100** | 删前**不** discard | 账页 key 变野指针 → 只能靠 `prune` 可达性剪枝清理;**DFS 途中会拿野 key 做 map 查找/比较** |

**实测结果**(2026-10-05,Debug + Release 双跑):

```
gcstress-x200  200 轮(含 prune 剪枝路径 100 轮) | 残留待回收累计=0 | 残留活组累计=0
               | 堆校验失败轮数=0 | WS 554.6→557.6 MB
[OK] 每轮回收彻底(残留待回收=0)
[OK] 每轮组全蒸发(残留活组=0)
[OK] CRT 堆全程无损坏(每轮 _CrtCheckMemory)
[OK] 无累积泄漏(WS 增长 3.0 MB,阈值 64 MB)
[SKIP] CRT 堆校验(Release CRT 不支持) ← Release 明确跳过,不冒充 OK
```

**结论覆盖范围(诚实标注边界)**:
- ✅ 已实证:组蒸发 → `m_retired` → `collectGarbage` 链路无堆损坏;
- ✅ 已实证:**prune 拿"已被数据侧释放的账页 key"做查找/比较是安全的**(路径 B,100 轮);
- ✅ 已实证:200 轮累积无泄漏(WS +3.0 MB,属分配器抖动量级)。
- ⚠ **仍未覆盖**:多层嵌套组的级联蒸发、地址复用幽灵、跨线程。
  跨线程的答案不是压测而是设计约束(见 C5 / 缺口 #2)。
- ⚠ ASan 仍是更强的证据(能报"读到了但没写坏"的野读);本条不能替代 ASan,
  只能作为**本机可复跑的持续保证**。有 ASan 环境时请补。

---

## 6. 协作与验证协议(建议)

1. **实现方**:每次改动 → ① 更新本文档对应条目(含验证状态)② 跑第 0 节整套回归 →
   ③ 在下方"变更记录"写一行。
2. **验证方**:以本文档为准,逐条挑**高风险/新增**条目独立复验(读码 + 跑命令 + 必要时写探针),
   结论写回本文档,并标明 `[实测]` 还是 `[论证]`。
3. **冲突处理**:文档与代码不一致时,**以能复跑出来的证据为准**,并立刻修正文档 ——
   历史上已出现过两次"注释与实现相反"(`m_retired` 的写入方、提示位图容量),
   这类不一致比缺注释更危险。
4. **不许做的事**:不把"理论上安全"写成"实测安全";不在没有证据的情况下把状态从
   `[论证]` 提升为 `[实测]`。
5. **推翻旧条目时保留推翻痕迹**:§1.6(Tester 边界)与缺口 #1/#6 都是把首版的错误论断
   标 `~~删除线~~` + 写明推翻理由,而不是静默改掉 —— 否则下一个 agent 会照着旧论断再踩一次。
6. **新增断言必须能证明"它能抓错"**:见 §4 末尾"分辨力"要求。

---

## 7. 变更记录

| 日期 | 变更 | 状态标记 |
|---|---|---|
| 首版 | 验证方建立事实基线:契约 C1~C9、不变量 8 条、性能/内存基线、回归基线、缺口清单 | 全部带验证状态 |
| 首版+10min | 实现方并行改动 `tests.cpp`(新增 T19)→ 修正 3 处漂移行号,并加入"行号必复核"纪律 | `[实测]` |
| **2026-10-05 第二轮(实现方)** | ① **T18 重写**:推翻"Tester 覆盖信号发错行"的错误论断(见 §1.6),改为捕获 `rowsAboutToBeRemoved` 参数断言 + 反例对照(比 `parent` 不比 `row`);② **T19 落地**:真造四层 `root→Outer→Inner→Only`,验证级联预判 + 带分辨力自检;③ 修 §5 缺口 #1 的恒真断言;④ 修 §5 缺口 #6(组地址复用现由 T17/Q 覆盖);⑤ `unit_tests` 加 `silenceCrtDialogs()`;⑥ §5 缺口 #5 作废(非 git 仓库)。行号已全部重新核对(C1~C9 引用的 15 处**全部未漂移**) | `[实测]` 19 块 / 62 条断言全绿;16/16 全量回归 |
| 2026-10-05 第二轮 | **当时未解决**:§5 缺口 #2(单线程前提无断言/开关)、#3(`m_retired` 峰值未入内存基准)、#4(`/W1`)、以及"ASan 无法初始化 → prune 内存安全仍是 `[论证]`" | 仍为待办 |
| **2026-10-05 第二轮(续·关闭缺口 #3)** | ① 引擎加 `m_retiredHighWater` 只增不减高水位 + `noteRetired()`(**所有 `m_retired` 写入必须走它**,直接 `push_back` 会漏记);② `MemBreakdown` 增 `retiredGroups / retiredHighWater / retiredBytes / totalWithRetired()`;③ 新增 `bench regroup` 场景 + 复现命令。**顺带修正 C4 的错误归因**:延迟释放的峰值**不是**来自 wipe-rebuild(实测恒为 0 —— 组被 `matchReusable` 复用、从不 cull),而是**换规则**(key 全变 → 复用全失配 → 旧组成片 cull) | `[实测]` 峰值 100000 组 ≈ **9.2 MB** = 稳定账本 77.1 MB 的 **11.9%**;`collectGarbage()` **3.6 ms**;规则可复原;**16/16 全量回归**(新增 `bench regroup`) |
| **2026-10-05 第二轮(续·ASan 替代)** | 新增 `bench gcstress`:200 轮反复"建组→裸删→只读触发蒸发→回收",**两条删除路径轮换**(A: `discard` 显式讣告 / B: 不 discard → 靠 `prune` 剪掉野 key),每轮 `_CrtCheckMemory()` + 累计泄漏曲线。把"prune 内存安全"从 `[论证]` 升为 `[实测]`(覆盖范围与未覆盖项见 §5)。⚠ 刻意让 **Release 显式打印 `[SKIP]` 而不是 `OK`** —— 不可用的检查绝不能冒充通过 | `[实测]` Debug 200 轮堆校验 **0 失败**、stderr 无 CRT 报告;两配置残留待回收/活组均为 0、WS +3.0 MB |
| **2026-10-05 第二轮(续·关闭缺口 #2)** | C5 逃生阀落地:①`setEpochOptimization(bool)` + `epochOptimizationEnabled()`;②关闭时**清零所有页 `checkedAt`**(让"关掉就一定全量枚举"可推理)+ `m_queryEpoch` 归零;③`crossThreadEntryCount()` + Debug 下 `onOutermostEntry()` 比对线程身份(检测到跨线程→stderr 报一次并提示调开关);④闸门条件改为 `m_epochOptimOn && checkedAt==epoch && valid`(**开关必须真接在闸门上,否则只是装饰**);⑤新增 `unit_tests` **T20** | `[实测]` **开/关投影树逐项相同**(`[组]Alum parts\|[组]Steel parts\|Sketch1\|` 两边一致);关闭态下数据侧删除仍被惰性发现。20 块 / 70 条断言全绿;**16/16 全量回归**(bench 场景增至 6 个:all/deep/dbg/mem/regroup/gcstress) |
| **2026-10-05 第三轮(关闭缺口 #4)** | `/W3` 落地:①CMakeLists 统一 `/utf-8 /W3`(不加 `/WX`),噪声按编号精确豁免;②**16 处 printf 格式串的 Unicode 符号 → ASCII**(理由:破坏 printf 分析导致 C4474,而屏蔽 C4474 会让参数检查失效);③C4819 误报做了 5 步调查取证后豁免(见 §1.8),BOM 方案验证无效已回退 | `[实测]` clean build **双配置 0 错误 0 警告** |
| **2026-10-05 第三轮(意外收获)** | 上 `/W3` 的过程中发现**验证基础设施自身有两个缺陷**:①11 处 `[CHECK]` 是裸 `printf`,**MISMATCH 不影响退出码** → 自动化回归抓不到(比没检查更危险);②`gcstress` 用**全局** `liveGroupCount()` 做**局部**断言,串跑时必然误报。两者都已修:统一 `checkOk()` + 退出码 = CHECK 失败数;gcstress 改用"进场景基线"断言。**新机制上线即抓出一个被 `return 0` 掩盖的真实 bug**:`bench dbg` 清僵尸时 `m_roots` 已空 → `prune` 直接 return → 残留组(`main` 场景一直守着挂探针根的规矩,`dbg` 漏了) | `[实测]` bench 6 模式 × 双配置 exit 全 0、`[CHECK!]` 全 0;**24/24 全量回归** |
| **2026-10-06(验证方复验 + 修假绿灯)** | 验证方独立复跑:六目标 × 双配置、bench 六模式全部确认;逐项核对 C4/C5/gcstress/账本分解的声明值,**全部吻合**(100000 组 × 96 B = 9.2 MB = 77.1 MB 的 11.9%;账本 88.4 MB;Debug gcstress 200 轮堆校验 0 失败)。**发现并修复一个假绿灯**:§0 的 `bench mem`/`bench regroup` 单跑时因缺 `cold-bind` 前置 → 读数恒 0 而 exit=0;已在 `bench.cpp` 加 `requireBoundShapeA()` 前置自检。另把全文 **21 处 `文件:行` 引用改为符号名**(它们已因文件增长全部漂移) | `[实测]` 六目标 × 双配置 exit 全 0;bench 六模式 × 双配置:**`all`/`deep`/`dbg`/`gcstress` exit 0 且 `[CHECK!]`=0,`mem`/`regroup` 单跑 exit 1 且 `[CHECK!]`=1(符合预期)**,共 22 项全对 |
| **2026-10-06(本补丁自曝的两处返工,留痕)** | ① **判据写错**:第一版前置判据用 `hasProjection(g_tree.root)`,但 `bindShapeA` 的 locate 只命中 A/P 前缀节点、**root 本身没有账页** → 正确的 `bench all` 被误判为缺前置,红 2 项;改为 `memoryBreakdown().pages > 0` 后恢复。② **提示串里的反引号**:第一版用 `` `bench all` ``,反引号在 C 字符串与 shell 转义下都不安全,已改纯引号文本 | `[实测]` 返工后 22/22 全对 |
| **2026-10-06(验证方复验实现方的补齐)** | 独立复跑实现方补齐后的前置自检,**逐项确认**:①**依赖场景 9/9 单跑均 exit=1 且提示可读**(`hot query model incr rebuild prune iloop mem regroup`);②**自足场景 5/5 单跑 exit=0,无误杀**(`cold wide gcstress deep dbg`);③**`cold,<场景>` 组合 9/9 全通**;④六目标 × 双配置 exit 全 0,`unit_tests` 20 块 / 70 条断言;⑤`bench all` × 双配置 `CHECK 失败数 = 0`。另**独立复现了上表的多处数值**(`parentOf+rowOf` 1881 ns/op、`delete-1k` 48663 ns/op、`prune-1M` 103 ns/op、`install-loop` 103 ms/install),与实现方记录同量级。补一条澄清:上表"单跑"列现已是**不可复现的历史值**(场景被自检拦截后不再输出测量行),已在表下加注说明 | `[实测]` 23 项场景级判定全对(9 依赖红 / 5 自足绿 / 9 组合绿),另加六目标 × 双配置 + `bench all` × 双配置 |
| **2026-10-06(验证方补:压力树 demo)** | 给 `interactive_demo` 加"压力树"(可选装,供手工把玩)+ **R 电池**自证。树规模实测:543 节点 = 506 Part + 25 Folder + 6 Mesh + 3 Geom + 3 Data;24 层深链;500 直属孩子的宽父亲;第二棵独立树。**过程中暴露并修掉 4 个"树构造"层面的坑**(都不是引擎缺陷,但都会让全树不变量变红,值得留痕):① 挂进 Mesh 的 Part 用 `PartObject(name,mat,nullptr)` ⇒ `parentForTree()==null` ⇒ `parentOf` 返回 null,正解是走有 `treeParent` 形参的构造(`MeshPart`);② **组 key 必须按页唯一** —— 多页各建同名 `Titan 件` 会被 `matchReusable` 按 key 互相 cull,被 cull 的组 owner 被 erase,其成员 `parentOf` 变 null(这条是**引擎既有语义的正确用法**,不是 bug);③ Mesh 节点必须也装页,否则它的孩子 `parentOf` 会跳过它;④ 安装走查必须"数据孩子 + 组再下钻一层",否则被组盖住的节点漏装 | `[实测]` 双配置 **0 错误 0 警告**;六目标 × 双配置 exit 全 0;`interactive_demo --selftest` **PASS(16 条 R 断言全绿)**;`bench all` × 双配置 `CHECK!=0` |
| **2026-10-06(验证方补:压力树控制台)** | 应"想在界面上直接操作这棵树"的诉求,给 `interactive_demo` 加**顶部压力树控制台**:一行按钮 + **选中节点投影坐标状态板**(投影父亲/行号/投影孩子 vs 原始孩子/自然父亲/存活组/待回收/纪元开关),并把 `log()`/`structuralReset()` 接上状态刷新。新增 **S 电池**无头驱动那三个规则按钮(平时只能鼠标点到),断言切换后全树 `parentOf/rowOf` 仍自洽、类型组确实建出、清除后透传且无重复。**过程中修掉一个自己的接线 bug**:控制台先于 `view/model` 创建,状态刷新里直接解引用 `g_view->selectionModel()` → 启动即 0xC0000005 访问违例(已加空指针守卫) | `[实测]` 双配置 0 错误 0 警告;`interactive_demo --selftest` PASS(R 16 + S 8) |
| **2026-10-06(引擎性能:位置索引 + 诚实的结论)** | **问题定位**(用户反馈"交互巨卡"):模型对每行都问 `parentOf`/`rowOf`,旧实现为给答案必须 `revalidate`→`liveMatchesLast` **全枚举该页孩子**;且**每次模型调用都是独立操作(各开纪元)**,页级校验无法跨调用复用 ⇒ 遍历 k 行 = k×O(k) = **O(k²)**。实测最宽层 5000 时 `parentOf` 8.59 µs;而**全量重算只要 1.2~2.8 ms**(占 1.5%,故重算侧无需优化)。**改法**:每页 `memberToPos[节点]=(投影父亲,行号)` + 全局 `m_nodePage[节点]=所属页`,`parentOf`/`rowOf` 先按页校验一次再查索引;`dropProjection` 按本页键集点查清理。**关键陷阱**(诊断实证):行号**不能**用自然父亲 `flat` 的位置 —— 被分组的成员不在自然父亲 flat 里(那里放的是组),其行号是"在所在组成员序列里的位置",必须成对存 (父亲,行号)。**⚠ 诚实结论**:索引让"查表"部分变 O(1),但**跨调用 O(k²) 没有消除** —— 实测(k=500/2000/5000/10000/20000 的一次全树遍历)0.35/5.16/32.4/121/485 ms,仍是超线性;计数探针证明 `N=10000` 遍历中数据侧枚举被调 **100,050,002** 次(每行 ≈k)。**要真正消除只有一条路**:数据侧提供变更信号(脏标记/版本号),使引擎不必靠枚举发现变化 —— 属数据层契约变更,不在本次范围。**两次尝试"跨操作复用成员表指针"均引入难以复现的陈旧读**(T16 在 Debug 稳定失败/Release 通过),已按正确性优先整段撤除。**内存代价**:`memberToPos` 74.3 MB + `m_nodePage` 65.9 MB,账本 88.4→**228.6 MB/百万被投影节点**,峰值工作集 299→**496 MB**。**回归护栏**:新增 **T21**(宽节点读侧 + 全树不变量),`unit_tests` 21 块 | `[实测]` 双配置 0 错误 0 警告;六目标 × 双配置 exit 全 0;`unit_tests` 21 块全绿(含 T16/T21);`bench all` × 双配置 `CHECK!=0` |
| **2026-10-06(第三方评审:入网前检视,发现并修复 R1/R2/R3)** | 评审方独立复验(clean build 双配置 0 错误 0 警告;15 项回归 exit 全 0;日志无 FAIL/CHECK!/MISMATCH)+ 写探针 `probe_review.cpp` 实证三个读码疑点。**坐实并修复三处**:① **R1(高危,正确性)**:`parentOf`/`rowOf` 的位置索引快路径在 `EpochScope` 之前执行 → 裸调用不递增纪元 → 快路径内 revalidate 把"上一操作校验过"误当"本操作校验过",数据侧静默变更后返回**陈旧坐标**(探针实证:删 L1 后裸 `rowOf(L3)` 答 1,真值 0)。生产触发面:连续多次精准删除之间无 rowCount 调用时,`beginRemoveRows` 拿到含死者的旧行号(gui_demo 因 Op 间有 dumpModel 而从未暴露)。修复 = 纪元在快路径前开(每操作首触页必重枚举,同操作内仍 O(1))。代价:`parentOf+rowOf` 959→**1342 ns/op**(旧值靠不健全跳过,作废);T21 Debug 诚实成本 ~13 s → **阈值改分配置**(Release 2 s/Debug 30 s,护栏仍能抓 ×600 的真爆炸)。② **R2(中危,内存)**:`dropProjection` 的 `m_nodePage` 清理键集收的是 flat+members 的**组键**(kv.first),漏掉各组成员(kv.second)→ 探针实证 6 条只清 2 条;答案正确性靠快路径查空回退兜底,但条目随组变更单调积累。修复 = 清理直接遍历 `memberToPos` 键集(与登记循环对称同源,不可能再漂移)。③ **R3(低危,契约字面)**:`walkBindings` 条件 `b.locate(n) && (maybeGroup ? !isGroup(n) : true)` 会对组节点**求值 locate**(探针实证 4 次)——违反不变量#3 的字面承诺,用户 locate 若对组做无防御 cast 即 UB。修复 = 先判非组再求值(位图门控收益保留)。回归固化:新增 **T22**(裸坐标新鲜度,分辨力:去掉 R1 修复必红)/ **T23**(拆页清理,分辨力:换回旧写法必红);`probe_review` 加入构建(第 7 目标)。**修复后全绿**:23 块 × 双配置、七目标回归、probe 0 confirmed、bench all/deep/dbg/gcstress/mem/regroup/cold-prune × 双配置 exit 全 0 | `[实测]` 探针修复前:P1 CONFIRMED(stale 1≠0)/P2 CONFIRMED(2/6)/P3 CONFIRMED(4 次);修复后全 OK |
| **2026-10-06(验证方补:启动即进压力树 + 形态澄清)** | 用户反馈"没看出是改造后的树"。**澄清:代码本来就是 model–view 形态**(`QTreeView` + `NavTreeModel : QAbstractItemModel`,模型只经 `ITreeSource` 读引擎,回归里有 `QAbstractItemModelTester`)。真正的问题是**压力树要手点按钮才出现**,默认看到的是初始树。修正:①**启动即构造压力树并切到它**(交互态无需点按钮);②控制台改为"显示压力树 / 显示初始树"**视图切换**(模型换 root,引擎同一套);③拆树后自动切回初始树(否则模型挂着已删 root);④主程序收尾补充压力树的政体感知清理;⑤新增 **`--smoke` 冒烟模式**,无头打印"界面此刻该显示什么"并点一次规则按钮 —— 实测 `模型 root = 压力装配体 / 存活组 = 18 / 根层 8 行 / 按钮 9 个`,切按类型后 `存活组 5 / 根层 5 行` | `[实测]` 双配置 **0 错误 0 警告**;六目标 × 双配置 + `--smoke` 全 exit 0;`bench all` × 双配置 `CHECK!=0` |

| **2026-10-07(v4 重构:脏通知契约 —— 用户拍板"放宽使用侧")** | 动机(用户):优化机械堆叠让代码杂乱难懂,进内网后无大模型支持难调试,且跨调用 O(k²) 仍未解决;愿意由使用侧承担"告诉引擎何时脏、哪里脏"的契约。设计与全部决策记录见 **docs/重构v4_脏通知.md**。**删除**(整套"猜数据变没变"的机械):纪元闸门(EpochScope/checkedAt/setEpochOptimization/跨线程检测)、liveMatchesLast 就地比对、全局 m_nodePage、每页 memberToPos、提示位图 m_pageHint/m_groupHint(C8 作废)。**新增**:①通知 API `invalidate/invalidateNode/notifyRemove/invalidateAll`(O(1) 置脏,批量天然合并);②每页 `dirty` 标志 —— 缓存检查塌缩成一行 `if (p.valid && !p.dirty) return;`;③单表 `entries`(node→{投影父亲,行号},合并 memberToGroup+memberToPos,随页同生共死 → R2 类泄漏结构上不可能);④`locate()` 统一下钻(parentOf/rowOf 共用);⑤三层漏通知兜底:prune/rebind 自愈(healIfDrift,双配置)+ paranoid 漏报点名(默认 Debug,`setParanoidChecks`,**只对干净页比对** —— 首版把已通知的脏页也点名,9 条误报,已修)+ structRev。**保留**:m_retired/collectGarbage(C1)、structRev(C9)、PtrSet、bindGen 豁免(简化为"同绑定→跳过")、ProjectionStages 七纯函数(buildFlat 改收 entries+owner)、组页自拆回查防护、异常安全闸门(valid 末尾置位,重算开始即撤销)。**迁移**:tests(T5/T8/T10/T16~T23;T20 换"漏报自愈"、T22 换"通知即新鲜"、T23 换 entries;新增"DataObject 构造不自动挂父"踩坑注释)、五 demo(收口点:createAndAttach/deleteChildRaw 包装自动通知;removeWithSignal/t5Remove 插 notifyRemove;S4/S8/Q 电池按新契约改写)、bench(incremental/dbg/gcstress 补通知;rebuild 通道**刻意保持静默**以压测 prune 自愈路径)、probe_review(三探针按 v4 契约重写,含"违约→陈旧→prune 自愈"负面验证)。**已知口径**:verifyGroups"应有组"在多层嵌套下按注册表计数,外层吞内层时顶层组数更小(M 电池"应有2 实际1"日志 v3 时代即如此,非失败,已加注释);prune-1M 113→202ms 是自愈比对的故意成本 | `[实测]` **七目标×双配置 exit 全 0、0 警告**;unit_tests 23 块×双配置;interactive 62 [OK]+--smoke;bench 全模式(all/deep/dbg/gcstress/cold,mem/cold,regroup/cold,prune)×双配置 CHECK=0;probe 0 confirmed(P1a 通知即新鲜/P1b 违约自愈/P2 entries 随页/P3 组上零求值);性能:rowcount 6.3×、wide-rowOf 3000×、model-dfs 2.6×、账本 −52%、跨调用 O(k²) 根除(T21 Debug 13s→1.06ms),对比表见 §3 |
| **2026-10-07(用户现场 bug:解除一个工厂组,整页组全灭)** | 诊断:**非引擎、非 Qt 视图** —— demo 层语义缺口。压力树"大平铺件"的 10 个材料组由 installRuleWalk 的工厂规则安装,不在 g_manual 登记;右键"删除组"(onDeleteGroup)只认手动登记 → erase 空转 → installGrouping 见"空登记"→ **uninstall 整页** → 10 组陪葬。修复(demo 层):① `g_dissolved`(owner 名 → 已解散 key 集)—— **工厂组解散 = 规则层排除**(组是规则产物,引擎没有"删单个组"概念,生产同此);② `composeSpec(owner)` 唯一合成点 = 手动规则(优先,不豁免组)+ 工厂规则 + 排除表,顺手修掉两个连带暗坑(工厂页上手动建组顶掉工厂组、onRebuild 工厂页重建后工厂组丢失);③ installGrouping v2:只有"无手动且无工厂"才 uninstall;④ 排除表按【名字】记 → 跨单父重建不复活,整棵压力树重装(installStressProjection)时清零保电池确定性。**修复中抓到的新坑(留痕)**:installRuleWalk 全树共享 `stressRuleFor(树根)` —— 所有页的组 key 前缀是【根名】("压力装配体/Steel"),不是页名;composeSpec 第一版用 stressRuleFor(owner) 合成出"页名/Steel",与在装页的 key 对不上 → 排除永远查不中(实测 10→10)。★ 教训:**合成规则必须与实装规则逐字节同源,前缀这种"看起来是页内实现细节"的东西就是同一性本身**。回归:R 电池新增 3 断言(10→9 其余无损 / 成员回落直属 / 解散态跨单父重建存活+其余 9 组重生) | `[实测]` interactive --selftest 双配置 PASS(66 [OK])、--smoke、unit_tests、probe_review 全 exit 0;双配置 0 错误 0 警告 |
| **2026-10-07(代码格式规范化:引擎六文件)** | 用户定三条规矩,引擎核心全部对齐:① **头文件函数注释统一 Doxygen**(`@brief/@param [in|out]/@return`,范本=revalidate/rawChildren 用户手改样式),覆盖 treeprojection.h / projection_stages.h / ptrset.h / itreesource.h / navtreemodel.h 全部函数;② **所有 if/while/for 一律换行加大括号**(含单语句 return/continue,范本=revalidate 函数体),消灭"一行挤三条"的读码地雷;③ **无语义变量全部更名**:p→page、kv→pageEntry/memberEntry、pit→pageIt、oit→ownerIt、mit→membersIt、git→groupPageIt、eit→posIt、n→node、g→group、c→child、mem→memberList、b→binding、vis→visibleNodes、oldAll→oldGroups、culled→culledGroups、deleted→removedGroups、stale→stalePages 等(cpp 注释按用户要求原样保留,仅同步注释里引用的变量名)。范围=引擎六文件(treeprojection.h/.cpp、projection_stages.h、ptrset.h、itreesource.h、navtreemodel.h);demo/tests/bench 未动。**纯格式与命名,零语义变化** | `[实测]` 双配置 clean build **0 错误 0 警告**;回归 12 项(Release:unit/nav/gui/tabs/inter/probe/bench + Debug:unit/inter/gui/tabs/nav)exit 全 0 |
| **2026-10-07(用户现场 bug:删一个组,其它组展开状态全被重置为展开)** | 诊断:**非引擎、非 Qt 缺陷,是 demo 视图层两处行为叠加** —— ① `endResetModel` 的 Qt 语义就是"所有 index 失效、视图丢弃全部展开状态"(生产"更新→reset"通道同样会遇到);② 旧 `structuralReset` 随后**无条件 `expandAll()`**,把手动折叠过的组也强行拉开。引擎侧恰恰是为保状态设计过的:`matchReusable` 按 key 复用旧 TreeGroup 指针(同 key 组跨重算指针稳定)+ C1 延迟回收保证 reset 括号内旧指针内存有效 —— 展开状态**有得保**,只是没被消费。修复(视图层标准配方):reset 前模型遍历收集展开节点【指针集合】→ reset 后遍历活投影树,只对"活树 ∩ 之前展开"恢复 `setExpanded(true)`,新节点默认折叠;删除无条件 expandAll(仅保留 视图初次切换/手动"展开全部"按钮/onRebuild 三处刻意全展开语义)。已知良性瑕疵:退役组地址被新组复用时会多展开一次(纯外观);生产要零误认改用业务 key 做恢复键,套路相同 | `[实测]` interactive --selftest 双配置 PASS、--smoke 通过、0 错误 0 警告 |
| **2026-10-07(空壳组修复 + 对接契约修正)** | ① **引擎修**:新增纯函数 `Pipeline::closeMembership`(Stage B-2b,迭代到不动点)—— 剔除"自己的 key 没有桶"的空壳组 + 丢掉随之变空的桶;`revalidate` 调用它,Debug 下 stderr 点名"规则输出自相矛盾"。起因:多层分组下"内层组失去全部成员,却仍被外层组按名字收着"→ 零成员空壳组。**真正的伤害不是多一个空行**:空壳只是 members 的 value、不是 key ⇒ 不在 `page.groups` ⇒ `dropProjection`/`uninstall`/`liveGroupCount()` 全部遍历不到它 ⇒ **静默泄漏**(实测 `uninstall` 后 `liveGroupCount()=0` 而归属表残留 1)。② **测试**:新增 **T24**(24 块),判据是恒等式 `memoryBreakdown().groupOwner == liveGroupCount()`;修复前 **3 条 FAIL**(实测"活组1/归属2""uninstall 后 0/1")⇒ 有分辨力;测试还当场抓出作者自己的场景 bug(漏给 C 出票)。③ **demo 根治**:`onDeleteGroup` 补"删除侧"引用清理 —— 把该组 label 从其它手动组成员名单里摘掉、并丢掉被摘空的登记(与 `doGroup` 建组时的"从其它组摘名字"对称;只做增侧清理 = 规则会输出自相矛盾的票)。④ **可读性**:`installGrouping` → **`syncPageRule`**(它实为"按当前登记把这一页重新投影一遍"),并补注释说明"删一个组为何走 `install` 而不是 `uninstall`"。⑤ **文档修正**:新增不变量 **#11**(成员表闭合)、C5 补"**属性漂移没有任何兜底**"警告(三层兜底比的都是孩子列表)、§1.5 补异常窗口(§516 换 members 而 §568 才换 groups,中间调用用户 `less`,抛异常即留下同 key 双胞胎 —— **这才是 `groupsKeptAsMember` 必须保留的真理由**,不是"多层存活路径")、§1.1 补通知 API 行、§3 补 paranoid 首发纪律、修掉三处 v4 遗留漂移(§1.5 checkedAt / §2#7 memberToGroup / §4 m_nodePage)。⑥ **bench 拆地雷**:`scenarioIncremental` 的 victims 由有放回抽样改 **`std::sample`**(无放回)—— 旧写法会对同一 leaf 二次 delete,`seed 123` 恰好 0 次重复掩盖了它,换随机 seed 约四成概率会撞。⑦ 同窗口另有两次 interactive 修复(工厂组解散只解散该组 `f624d5f`、结构变更跨 reset 保展开态 `e0a4fed`),详见各自提交 | `[实测]` 七目标 × 双配置 exit 全 0、**0 错误 0 警告**;`unit_tests` **24 块** × 双配置;`interactive --selftest` 66 [OK] PASS;bench 全模式(all/deep/dbg/gcstress/cold,prune/cold,mem/cold,regroup)× 双配置 `CHECK=0`;probe **0 confirmed**;Release bench **TOTAL 2288.9 ms / WS 326 MB / 账本 108.6 MB**(与 §3 基线一致,无回归);新诊断首轮全量回归中**仅在 T24 的设计场景命中一次**,其余 23 块 + 七 demo + bench 零噪声 |
| **2026-10-07(回收粒度:探针 P4 实证的 release 通道 UAF)** | **发现**:写 P4 探针检验 C1 的防线,在"**守约但不带括号**的调用"路径上**坐实一个 UAF** —— `notifyRemove` 的调用前提本就没有括号要求(§1.1),之后一次纯读触发组蒸发入 `m_retired`;此时**任何一次不相干的精准删除**都会走 `endRowRemoval()` → 整队列 `collectGarbage()` → 把这个"从未被任何信号覆盖过"的组 release 掉。而 `endRemoveRows()` **只作废该 parent 下 `[first,last]` 行的** persistent index ⇒ 视图侧仍认为它有效。**P4 实测**:`retired 1→0`(组确实被释放)而 `held.isValid()==1`(Qt 从没被告知)⇒ `internalPointer` 悬空,之后任何 `rowCount(held)`/展开态检查都是 UAF。**根因不是"忘了通知",是两个回收点与两种坐标作废范围不匹配**(reset 作废全部 / removeRows 只作废范围),而 `collectGarbage()` 一律清整队列。**修复**:① 窄接口新增 `retiredCount()`(水位)+ `collectGarbageSince(mark)`;② `NavTreeModel::beginRowRemoval` 记水位、`endRowRemoval` 只回收本窗口新退役的那一段(reset 通道仍用整队列 `collectGarbage`,因为 `endResetModel` 作废全部坐标);③ **`discard` 改延迟释放**(`deferDelete=true`)—— 它是 `notifyRemove` 的内含步骤而后者不要求括号,当场 delete 组就是同一类 UAF;册页仍立即拆掉(按死亡指针做键,不能留);④ 契约定稿:C2 修订、C3 改写为"两个粒度必须匹配坐标作废范围"、§1.1 表补两行并把调用时机写成"**`end` 之后**"(作废发生在 end 不是 begin)。**新增回归**:`unit_tests` **T25**(`closeMembership` 纯函数:空输入/已闭合/单壳/宿主只剩空壳/多层依赖 —— 最后一条是"改成单轮过滤"的红靶子);`probe_review` **P4**(两段判据:P4a 不得越窗回收、P4b reset 通道必须"既释放又作废坐标")。**分辨力**:修复前 P4 CONFIRMED,修复后 P4a/P4b 皆 OK;T25 的 ④ 在单轮实现下必红 | `[实测]` P4 修复前 `retired 1→0 & held.isValid=1`(CONFIRMED)、修复后 `retired 1→1`(残留留着)与 reset 后 `retired=0 & held.isValid=0`;七目标 × 双配置 exit 全 0、0 错误 0 警告;`unit_tests` **27 块** × 双配置;interactive 66 [OK];bench 全模式 × 双配置 `CHECK=0`;probe **0 confirmed** |
| **2026-10-07(release 分支:测试提速 / 命名 / 覆盖率)** | 三件事,全部服务于"能交给内网"。① **测试提速**:`bench` 拆**规模两档** —— 默认快档(Shape A 5,111 节点 / 110 页,形状完全不变)、`bench full` 复现 §3 基线原规模(1,010,101 节点)。断言纪律随之收紧:**不许写死节点数**,一律用 `g_scale` 派生量(写死即换档变红)。实测:七目标合计墙钟 **Release 1.70 s / Debug 7.66 s**(改前仅 bench 一项就 58 s / 321 s);`bench full` 仍原样复现 §3 的绝对读数(账本 108.6 MB / 100000 组 / 9.2 MB 峰值),`bench full` 自身 57 s / CHECK=0。快档下"缺前置自检"契约 23 项判定全对(9 依赖 exit=1 / 5 自足 exit=0 / 9 组合 exit=0)。② **改名**:`Pipeline` → **`ProjectionStages`**,文件 `pipeline.h` → **`projection_stages.h`** —— 内网反馈"Pipeline 语义过泛";新名按"领域 + 角色"取,与 `TreeProjection` 同一家族,一看便知是"投影的一步"而非通用设施。③ **行覆盖率(新增可复跑测量)**:`python tools/coverage/run_coverage.py` —— MinGW g++ 13.1 + gcov,靠"假 QObject 遮蔽"+"跨 TU 逐行取并集"两个机关,**交付代码一行不改**。实测引擎 **94.3%**(796/844);`navtreemodel.h` 需真实 Qt 运行库,本机不可插桩(由 MSVC 侧 T15/T16/T18/T19 覆盖)。**覆盖率工作换来两个真实修复**:★ `PtrSet::reserve` 静默丢数据(旧实现换表不重插元素、`m_count` 与表内容脱钩 —— 此前只被构造函数的空表调用掩盖着,已修并钉在 T27);★ 电池从未碰过的接口面(`apply`/`refresh`/静态 pool/`invalidateNode`/`invalidateAll`/`collectGarbageSince`/宽亲槽位表/塌缩清桶/等长内容漂移)现由 **T26/T27** 覆盖(unit_tests **27 块**)。④ 为了让测试能在无 Qt 编译器下构建,`tests.cpp` 加了 `NT_ENGINE_ONLY` 守卫(排除 T15/T16/T18/T19)—— **MSVC 侧行为完全不变**(27 块全绿)。| `[实测]` 七目标 × 双配置 exit 全 0、0 错 0 警;unit_tests **27 块**;interactive 66 [OK];bench 全模式(fast + `full`)CHECK=0;probe 0 confirmed;覆盖率脚本 exit=0(94.3% ≥ 85%) |
| **2026-10-07(可读性:Projection::groups → ownedGroups)** | 改名,零语义改动。**动机**:`groups` 这个名字读起来像"本层顶层摆着的组"(与 `flat` 平行),实际语义是**"本页【拥有】的全部组"** —— 含**嵌在别的组肚子里的嵌套组**(它们一行都不在 `flat` 里,只出现在收养者的成员表里),但账页仍归本页管。级联拆页 / 传递置脏 / 记账问的都是"归属",所以遍历它是对的;而把它当"这页显示了几行"就是错的。**做法**:成员改 `ownedGroups`,并在 `Projection` 结构体头部加一段【三个正交关系】的说明 —— 位置(position,`flat`/`entries[].row`)、容器(containment,`entries[].container`)、归属(ownership,`ownedGroups`)各占各的字段,混在一起正是这个结构最难读的地方;字段旁的注释列出四个消费者各自问的是哪个问题。`MemBreakdown::groupsVec/groupsBytes` 同步改为 `ownedGroupsVec/ownedGroupsBytes`(bench 的账本分解表同步)。⚠ 三份**带日期的设计归档**(`设计精讲.md` / `重构v4_脏通知.md` / `性能实测_百万节点.md`)里仍写着 `p.groups` —— 它们记录的是当时的代码,按本仓库"保留推翻痕迹"的规矩**不改**;读它们时请按新名对照。| `[实测]` 七目标 × 双配置 exit 全 0、0 错 0 警;覆盖率脚本 exit=0(94.3%);手册片段检查通过 |
