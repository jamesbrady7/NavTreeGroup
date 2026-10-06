// ═══════════════════════════════════════════════════════════════
//  bench.cpp —— TreeProjection 百万节点性能压测(控制台,Release 跑)
// ═══════════════════════════════════════════════════════════════
//  目标:回答"这套惰性投影方案能不能扛住生产环境的百万节点"。
//
//  用法:
//    bench.exe            跑全部常规场景(build/cold/hot/query/model/
//                         incremental/rebuild/wide/prune/install-loop)
//    bench.exe deep       深链场景单跑(递归深度风险,可能崩,单独进程隔离)
//    bench.exe <名字>     单跑某场景
//
//  树形状(贴近 CAD 装配树):
//    Shape A 层级树:root → 100 装配(A###) → 100 零件(P###) → 100 特征(F######)
//                   共 1,010,101 节点;投影装在 A 层与 P 层(按材料 10 类分组)
//    Shape B 宽树:  单父 200,000 直属孩子(压 bucketByKey/rowOf/firstVis)
//    Shape C 深链:  100,000 级链(压 DFS 递归深度,deep 场景单跑)
//
//  政体:全部 BenchNode = m_children 连坐政体(最简;政体差异被虚接口吸收,
//  对引擎侧计时没有影响 —— 引擎只消费 visibleChildrenCount/visibleChild)。
#include <QElapsedTimer>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>   // checkOk 的可变参数
#include <string>
#include <vector>
#include <map>
#include <random>
#include <unordered_set>   // pbreak A/B 对照用(不再从 treeprojection.h 传递包含)
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <crtdbg.h>
#pragma comment(lib, "psapi.lib")   // GetProcessMemoryInfo(工作集监控)
#endif
#include "guibase.h"
#include "treeprojection.h"
#include "navtreemodel.h"

// ── 压测节点:名字 + 材料,m_children 政体 ─────────────
class BenchNode : public GuiBase {
public:
    BenchNode(GuiBase* p, std::string name, std::string mat = {})
        : GuiBase(p), m_name(std::move(name)), m_mat(std::move(mat)) {
        if (p) p->addTreeChild(this);   // 挂进父亲的 m_children(连坐政体)
    }
    std::string type() override { return "Bench"; }
    const std::string& name() const { return m_name; }
    const std::string& material() const { return m_mat; }
private:
    std::string m_name;   // 名字短(SSO 内,无堆分配):A###/P###/F######
    std::string m_mat;    // 材料 key(10 类循环)
};

static TreeProjection& P = TreeProjection::instance();

// ── 计时/内存工具 ─────────────────────────────────────
static double workingSetMB() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / (1024.0 * 1024.0);
#endif
    return 0.0;
}
struct BenchResult { const char* name; double ms; long long ops; };
static std::vector<BenchResult> g_results;

// ── CHECK 机制:失败必须反映在【退出码】里 ──────────────────────
// ★ 为什么要专门做这个(实测踩到):原先 11 处 `[CHECK] … OK/MISMATCH` 全是
//   裸 printf,MISMATCH 只打在屏幕上,exit code 照样是 0 —— 于是
//   "gcstress 报 MISMATCH 但 bench all exit=0",自动化回归**完全抓不到**。
//   这比没有检查更危险:它让人以为检查过了。
//   约定:所有 CHECK 一律走 checkOk();g_checkFail > 0 ⇒ main 返回 1。
static int g_checkFail = 0;
// ok=false 时记一次失败,并把行首的 [CHECK] 换成 [CHECK!] 便于肉眼定位
static void checkOk(bool ok, const char* fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok) ++g_checkFail;
    printf("%s %s\n", ok ? "[CHECK ]" : "[CHECK!]", msg);
}

// 跑一段计时:name=场景名,ops=该场景的核心操作次数(算 ns/op 用)
template <class F>
static void bench(const char* name, long long ops, F&& f) {
    double ws0 = workingSetMB();
    QElapsedTimer t; t.start();
    f();
    double ms = (double)t.nsecsElapsed() / 1e6;   // ns 精度
    double ws1 = workingSetMB();
    g_results.push_back({name, ms, ops});
    if (ops > 0)
        printf("[BENCH] %-22s %10.1f ms  %12lld ops  %8.1f ns/op  WS %.0f->%.0f MB\n",
               name, ms, ops, ms * 1e6 / (double)ops, ws0, ws1);
    else
        printf("[BENCH] %-22s %10.1f ms  %26s  WS %.0f->%.0f MB\n",
               name, ms, "", ws0, ws1);
    fflush(stdout);
}

// ── Shape A:层级树 root → 100A → 100P → 100F = 1,010,101 节点 ──
static const int NA = 100, NP = 100, NF = 100;   // 装配×零件×特征
static const char* MATERIALS[10] = {
    "Steel", "Alum", "Copper", "Plastic", "Rubber",
    "Glass", "Wood", "Titan", "Brass", "Ceramic"
};
struct ShapeA {
    BenchNode* root = nullptr;
    std::vector<BenchNode*> internals;   // A 层 + P 层(装投影的对象)
    std::vector<BenchNode*> leaves;      // F 层(百万叶子,抽样查询用)
};
static ShapeA g_tree;

static void buildShapeA() {
    bench("build-1M", 1010101, [] {
        g_tree.root = new BenchNode(nullptr, "ROOT");
        for (int a = 0; a < NA; ++a) {
            auto* asmNode = new BenchNode(g_tree.root, "A" + std::to_string(a));
            g_tree.internals.push_back(asmNode);
            for (int p = 0; p < NP; ++p) {
                auto* part = new BenchNode(asmNode, "P" + std::to_string(a * NP + p));
                g_tree.internals.push_back(part);
                for (int f = 0; f < NF; ++f) {
                    // 叶子占采样即可,全存 vector 会吃 8MB 指针 —— 值得,查询场景要用
                    auto* leaf = new BenchNode(part, "F" + std::to_string(f),
                                               MATERIALS[(a + p + f) % 10]);
                    g_tree.leaves.push_back(leaf);
                }
            }
        }
    });
}

// 定位谓词:A/P 层(内部节点)装投影;按材料分组
static void bindShapeA() {
    P.registerRoot(g_tree.root);
    TreeProjection::SpecBinding b;
    b.id = "bench-material";
    b.locate = [](GuiBase* n) {
        auto* bn = dynamic_cast<BenchNode*>(n);
        return bn && !bn->name().empty() && (bn->name()[0] == 'A' || bn->name()[0] == 'P');
    };
    b.spec.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* bn = dynamic_cast<BenchNode*>(n);
        return bn ? bn->material() : std::string();
    });
    b.spec.opt.dynamicAll = true;
    P.bindSpec(std::move(b));   // bindSpec 内部立刻 rebind(计时含首次全量)
}

// ── 场景实现 ──────────────────────────────────────────

// ★ 前置自检(验证方 2026-10-06 提出,实现方补齐覆盖面):
//   【哪些场景依赖 bindShapeA 已装好】= 凡是读账本/测组内行为的:
//     hot(query 的缓存命中) · query(组内定位) · model · incr · rebuild ·
//     prune · iloop · mem · regroup
//   【不依赖】(自带树或自建投影,单跑有意义):cold · wide · gcstress · deep · dbg
//   为什么必须有:`main` 的派发是 `bool wantCold = wants("cold") || wants("pbreak")`,
//   单跑 `bench prune` 之类它不成立 → 没装绑定 → 场景静默跑成 0,而 exit 仍是 0。
//   ⚠ 最隐蔽的是 prune:账本空 → 无页可剪 → **0.0 ns/op**,数字"看起来合理"却完全
//     没测到东西。实测单独跑 vs 有账本:
//       rowcount-cache-hit  100.8 → 429.7 ns/op (4.3×)
//       parentOf+rowOf      466.8 → 1856.2 ns/op (4.0×)
//       delete-1k+revalidate 954.6 → 50466.6 ns/op (**53×**)
//       prune-1M              0.0 → 107.0 ns/op (根本没测到)
//   所以缺前置的判据是"这次测量无效",不是"这次数值异常"—— 靠看数字发现不了,
//   只能靠前置自检。返回 true = 前置满足,可以继续。
//   ⚠ 判据【不能】用 hasProjection(g_tree.root):bindShapeA 的 locate 只命中
//     名字以 A/P 开头的节点,root 自己【没有】账页 —— 用 root 判会把正常情况
//     误判成缺前置(补丁第一版就是这么错的,bench all 立刻变红 2 项)。
//   正确判据 = "账本里有账页"(pages > 0),它直接对应这两个场景要读的东西。
static bool requireBoundShapeA(const char* scenario) {
    if (!g_tree.root || g_tree.internals.empty()) {
        checkOk(false, "%s 需要先建树(Shape A 不存在)。请用 `bench all`", scenario);
        printf("[SKIP] %s:Shape A 未建立 —— 判定为【失败】而非通过\n", scenario);
        fflush(stdout);
        return false;
    }
    if (P.memoryBreakdown().pages > 0) return true;
    checkOk(false,
            "%s 需要先装绑定(账本为空时读数恒为 0,不是真结果)。"
            "请用 bench all 或 bench cold,%s", scenario, scenario);
    printf("[SKIP] %s:账本为空(缺前置 cold-bind),已判定为【失败】而非通过 —— "
           "详见本函数注释\n", scenario);
    fflush(stdout);
    return false;
}

// 冷启动:bindSpec + rebind,对 ~10,100 个内部节点装投影(每个 100 孩子分 10 组)
static void scenarioCold() {
    QElapsedTimer t; t.start();
    bindShapeA();
    double ms = (double)t.nsecsElapsed() / 1e6;
    g_results.push_back({"cold-bind-10k-parents", ms, (long long)g_tree.internals.size()});
    printf("[BENCH] %-22s %10.1f ms  %12lld parents  %8.1f ns/parent  (含 rebind 全树 DFS + prune)\n",
           "cold-bind", ms, (long long)g_tree.internals.size(),
           ms * 1e6 / (double)g_tree.internals.size());
    // 校验:活组数 = P 层数 × 10(A 层孩子是 P,material 为空 → byKey 出不了票,
    // A 层 0 组是正确行为,不是丢组)
    size_t expect = (size_t)NA * NP * 10;
    size_t got = P.liveGroupCount();
checkOk(got == expect, "liveGroupCount = %zu (expect %zu)", got, expect);
    fflush(stdout);
}

// 热路径:childCount 缓存命中(模型 rowCount 每次展开/重绘都打这条)
static void scenarioHot() {
    if (!requireBoundShapeA("hot")) return;   // ★ 见 requireBoundShapeA 的注
    std::mt19937 rng(42);
    const int ROUNDS = 20000;
    bench("rowcount-cache-hit", ROUNDS, [&] {
        for (int i = 0; i < ROUNDS; ++i) {
            GuiBase* n = g_tree.internals[rng() % g_tree.internals.size()];
            volatile size_t c = P.childCount(n);   // 命中缓存:拉活列表+指针序列比对
            (void)c;
        }
    });
}

// 账本内存分解:回答"引擎那 ~130 MB 花在哪"。
// 内网做内存预算时需要的是这张表,不是"总计约 130 MB"这种估值。
static void scenarioMem() {
    if (!requireBoundShapeA("mem")) return;   // ★ 账本为空时读数是 0,不能冒充通过
    auto mb = P.memoryBreakdown();
    auto mbT = [](size_t b) { return (double)b / (1024.0 * 1024.0); };
    printf("\n======== 账本内存分解 ========\n");
    printf("  条目数:\n");
    printf("    账页(m_byParent)          %10zu\n", mb.pages);
    printf("    存活组                    %10zu   (= m_groupOwner)\n", mb.groupOwner);
    printf("    分组成员指针(memberPtr)    %10zu\n", mb.memberPtr);
    printf("    entries 条目(位置单表)   %10zu\n", mb.entries);
    printf("    lastLive 指针             %10zu\n", mb.lastLivePtr);
    printf("    flat 指针                 %10zu\n", mb.flatPtr);
    printf("  字节(估算口径见 treeprojection.cpp memoryBreakdown):\n");
    printf("    m_byParent 本体+桶        %10.1f MB\n", mbT(mb.pageObjBytes));
    printf("    每页 spec                 %10.1f MB\n", mbT(mb.specBytes));
    printf("    lastLive(缓存钥匙)        %10.1f MB\n", mbT(mb.lastLiveBytes));
    printf("    flat(投影序列)            %10.1f MB\n", mbT(mb.flatBytes));
    printf("    p.members(组->成员)        %10.1f MB\n", mbT(mb.membersBytes));
    printf("    entries(位置单表)         %10.1f MB   (v3 时这里是三张表:\n",
           mbT(mb.entriesBytes));
    printf("      v3 对照:memberToGroup 54.1 + memberToPos 74.3 + nodePage 65.9 = 194 MB)\n");
    printf("    p.groups + m_groupOwner   %10.1f MB\n",
           mbT(mb.groupsBytes + mb.groupOwnerBytes));
    printf("    ------------------------------------\n");
    printf("    引擎账本小计              %10.1f MB\n", mbT(mb.totalBytes()));
    printf("    (另:TreeGroup 对象本体    %10.1f MB)\n", mbT(mb.groupObjBytes));
    printf("    -- 延迟释放(C4)-------------\n");
    printf("    当前待回收组             %10zu  (%.1f MB)\n",
           mb.retiredGroups, mbT(mb.retiredBytes));
    printf("    待回收峰值(高水位)       %10zu  (%.1f MB)\n",
           mb.retiredHighWater, mbT(mb.retiredHighWater * 96));
    printf("    峰值合计(账本+延迟上界)  %10.1f MB\n", mbT(mb.totalWithRetired()));
    fflush(stdout);
}

// 内存分解 + 单节点字节成本:移植时"每 100 万节点要多少内存"的换算依据
static void scenarioMemAndRates() { scenarioMem(); }
// 随机查询:parentOf + rowOf(模型 parent()/indexFor 的热路径)
static void scenarioQuery() {
    if (!requireBoundShapeA("query")) return;   // ★ 组内定位是本场景的测点,必须先有组
    std::mt19937 rng(7);
    const int N = 100000;
    bench("parentOf+rowOf", N, [&] {
        for (int i = 0; i < N; ++i) {
            GuiBase* leaf = g_tree.leaves[rng() % g_tree.leaves.size()];
            GuiBase* par = P.parentOf(leaf);       // 应命中材料组
            volatile int r = P.rowOf(leaf);        // 组内 100 成员线性扫
            (void)par; (void)r;
        }
    });
    // ── 归因诊断(常驻):拆开 parentOf / rowOf,并给出查表结构对照 ──
    // 这三项是本场景全部成本的来源,引擎任何一次改动都可能让某一项劣化,
    // 所以常驻打印而不是跑完就删(它们本身耗时 <100 ms,不影响总时长)。
    {
        std::mt19937 r2(7);
        QElapsedTimer t; t.start();
        for (int i = 0; i < N; ++i) {
            GuiBase* leaf = g_tree.leaves[r2() % g_tree.leaves.size()];
            volatile GuiBase* p = P.parentOf(leaf); (void)p;
        }
        double ms = (double)t.nsecsElapsed() / 1e6;
        printf("[Q] parentOf only            %8.1f ms  (%.0f ns/op)\n", ms, ms * 1e6 / N);
        fflush(stdout);
    }
    {
        std::mt19937 r3(7);
        QElapsedTimer t; t.start();
        for (int i = 0; i < N; ++i) {
            GuiBase* leaf = g_tree.leaves[r3() % g_tree.leaves.size()];
            volatile int r = P.rowOf(leaf); (void)r;
        }
        double ms = (double)t.nsecsElapsed() / 1e6;
        printf("[Q] rowOf only (含 parentOf) %8.1f ms  (%.0f ns/op)\n", ms, ms * 1e6 / N);
        fflush(stdout);
    }
    {
        // 查表结构对照:10k 键(引擎账本页数)下 std::map vs unordered_map 的随机查找
        // 这是引擎选型的依据 —— 红黑树每级一次可能 cache miss 的指针追逐。
        std::vector<GuiBase*> keys;
        keys.reserve(10100);
        for (GuiBase* n : g_tree.internals) keys.push_back(n);
        std::map<void*, int> mp; std::unordered_map<void*, int> um;
        for (size_t i = 0; i < keys.size(); ++i) { mp[keys[i]] = (int)i; um[keys[i]] = (int)i; }
        std::mt19937 r4(9); volatile int sink = 0;
        const int M = 1000000;
        QElapsedTimer t; t.start();
        for (int i = 0; i < M; ++i) sink += mp.find(keys[r4() % keys.size()])->second;
        double msMap = (double)t.nsecsElapsed() / 1e6;
        t.restart();
        for (int i = 0; i < M; ++i) sink += um.find(keys[r4() % keys.size()])->second;
        double msUn = (double)t.nsecsElapsed() / 1e6;
        printf("[Q] lookup 10k keys: std::map %6.0f ns/find | unordered_map %6.0f ns/find\n",
               msMap * 1e6 / M, msUn * 1e6 / M);
        (void)sink;
        fflush(stdout);
    }
}

// 模型全量 DFS:模拟 QTreeView 全部展开(每个节点 rowCount+index+parent)
static void scenarioModel() {
    if (!requireBoundShapeA("model")) return;   // ★ CHECK 断言 visited > 原始节点数,靠分组才成立
    NavTreeModel model(g_tree.root, P);
    long long visited = 0;
    bench("model-full-dfs", 1010101, [&] {
        // 显式栈 DFS(递归 1M 层会爆栈,视图本身也是逐层拉)
        std::vector<QModelIndex> stack;
        stack.push_back(QModelIndex());   // 不可见根
        while (!stack.empty()) {
            QModelIndex cur = stack.back(); stack.pop_back();
            int rc = model.rowCount(cur);
            for (int i = 0; i < rc; ++i) {
                QModelIndex c = model.index(i, 0, cur);
                if (!c.isValid()) continue;
                ++visited;
                // parent() 每次都被视图调:parentOf 下钻 + rowOf 线性扫
                QModelIndex back = model.parent(c);
                Q_ASSERT(back == cur || rc == 0);
                stack.push_back(c);
            }
        }
    });
    checkOk(visited > 1010101, "model dfs visited = %lld (投影后应 > 原始节点数,含组)", visited);
    fflush(stdout);
}

// 增量:数据侧删 1000 个散点叶子 → 引擎不感知,下次查询现场收敛
static void scenarioIncremental() {
    if (!requireBoundShapeA("incr")) return;   // ★ 增量删除的代价几乎全在账页重算上
    std::mt19937 rng(123);
    const int N = 1000;
    std::vector<BenchNode*> victims;
    for (int i = 0; i < N; ++i) victims.push_back(g_tree.leaves[rng() % g_tree.leaves.size()]);
    bench("delete-1k+revalidate", N, [&] {
        for (BenchNode* v : victims) {
            GuiBase* par = P.parentOf(v);          // 事前坐标(精准通道语义)
            int row = P.rowOf(v);
            (void)row;
            GuiBase* dataPar = v->parentForTree();
            P.notifyRemove(dataPar, v);            // ★v4:invalidate(父页)+discard(死者)
            dataPar->removeTreeChild(v);
            delete v;
            volatile size_t c = P.childCount(par); // 触发受影响父亲 revalidate
            (void)c;
        }
    });
}

// 全量重建通道:wipe 全部叶子 → 重建 → prune + rebind(生产政体)
static void scenarioRebuild() {
    if (!requireBoundShapeA("rebuild")) return;   // ★ CHECK 期望 10 万组重生,账本空则恒失败
    // ★ 从场景起点重置,这样量到的就是"本次重建通道"独有的峰值,不掺上一场景的残留
    //   (PROJECT.md §5 缺口 #3 / 契约 C4)。
    P.resetRetiredHighWater();
    size_t retiredAtStart = P.retiredCount();
    // wipe:删掉全部剩余叶子(从 internals 里挑 P 层清孩子)
    // ★ 本场景刻意【保持静默删除】(只 discard 叶子,不 invalidate 父页):
    //   全量重建通道的生产契约就是 "wipe → prune → rebuild → rebind" ——
    //   prune/rebind 是卫生边界,漏通知的陈旧页在边界上全量自愈(healIfDrift)。
    //   这里同时压测的就是自愈路径本身的成本。
    QElapsedTimer t; t.start();
    for (BenchNode* n : g_tree.internals) {
        if (n->name()[0] != 'P') continue;
        while (n->visibleChildrenCount() > 0) {
            GuiBase* c = n->visibleChild(0);
            P.discard(c);
            n->removeTreeChild(c);
            delete c;
        }
    }
    double wipeMs = (double)t.nsecsElapsed() / 1e6;
    // rebuild:每个 P 层重挂 100 个新叶子(新地址 → 全量重建政体的真实压力)
    t.restart();
    long long rebuilt = 0;
    for (BenchNode* n : g_tree.internals) {
        if (n->name()[0] != 'P') continue;
        for (int f = 0; f < NF; ++f) {
            // ⚠ 曾错写 (rebuilt+f)%10:rebuilt 每叶自增,与 f 叠加 → 材料索引步进 2
            //   → 只命中 5 种偶数位材料,引擎被冤枉组出 5 组/P(bench 自己的 bug)
            new BenchNode(n, "F" + std::to_string(f), MATERIALS[f % 10]);
            ++rebuilt;
        }
    }
    double rebuildMs = (double)t.nsecsElapsed() / 1e6;
    // prune + rebind:引擎侧重生(拆开计时定位成本)
    t.restart();
    P.prune();    // 冷 prune:1 万个页全 MISS,重算流水线
    double pruneColdMs = (double)t.nsecsElapsed() / 1e6;
    t.restart();
    P.prune();    // 暖 prune:全 HIT,纯 DFS + 就地比对
    double pruneWarmMs = (double)t.nsecsElapsed() / 1e6;
    t.restart();
    P.rebind();   // = 暖 prune + walkBindings(1M DFS + 1 万次 installInternal 强制重算)
    double rebindMs = (double)t.nsecsElapsed() / 1e6;
    printf("[BENCH]   rebind 拆分:prune冷 %7.1f ms | prune暖 %7.1f ms | rebind(暖prune+walk) %7.1f ms -> walk ~ %.1f ms\n",
           pruneColdMs, pruneWarmMs, rebindMs, rebindMs - pruneWarmMs);
    // 生产口径的通道成本 = wipe + rebuild + (prune + walk) —— rebind 的内芯
    double walkMs = rebindMs - pruneWarmMs;
    double chanMs = wipeMs + rebuildMs + pruneColdMs + walkMs;
    g_results.push_back({"rebuild-channel", chanMs, rebuilt});
    printf("[BENCH] %-22s wipe %7.1f + rebuild %7.1f + prune冷 %7.1f + walk %7.1f = %8.1f ms (%lld leaves)\n",
           "rebuild-channel", wipeMs, rebuildMs, pruneColdMs, walkMs, chanMs, rebuilt);

    size_t expect = (size_t)NA * NP * 10;   // 只有 P 层出组(A 层孩子无材料)
checkOk(P.liveGroupCount() == expect, "after rebuild liveGroupCount = %zu (expect %zu)",
           P.liveGroupCount(), expect);
    // ── 全尺度探针:P 层逐节点体检(此时缓存新鲜,childCount 只读) ──
    size_t withProj = 0, cc10 = 0, cc0 = 0, ccOther = 0, leafSum = 0;
    for (BenchNode* n : g_tree.internals) {
        if (n->name()[0] != 'P') continue;
        if (P.hasProjection(n)) ++withProj;
        size_t cc = P.childCount(n);
        leafSum += n->visibleChildrenCount();
        if (cc == 10) ++cc10; else if (cc == 0) ++cc0; else ++ccOther;
    }
    printf("[PROBE] P nodes: hasProj=%zu/10000  childCount==10:%zu  ==0:%zu  other:%zu  rawLeaves=%zu\n",
           withProj, cc10, cc0, ccOther, leafSum);
    // 解剖首个非 10 组的 P:打印 flat 内容
    for (BenchNode* n : g_tree.internals) {
        if (n->name()[0] != 'P') continue;
        size_t cc = P.childCount(n);
        if (cc == 10) continue;
        printf("[PROBE] %s childCount=%zu flat:", n->name().c_str(), cc);
        for (size_t i = 0; i < cc; ++i) {
            GuiBase* x = P.child(n, i);
            if (auto* g = dynamic_cast<TreeGroup*>(x))
                printf(" [%s:%zu]", g->ticket().key.c_str(), P.childCount(g));
            else if (auto* bn = dynamic_cast<BenchNode*>(x))
                printf(" %s/%s", bn->name().c_str(), bn->material().c_str());
            else printf(" ?");
        }
        // 原始孩子材料分布
        std::map<std::string,int> dist;
        for (size_t i = 0; i < n->visibleChildrenCount(); ++i)
            dist[dynamic_cast<BenchNode*>(n->visibleChild(i))->material()]++;
        printf(" | raw material dist:");
        for (auto& kv : dist) printf(" %s×%d", kv.first.c_str(), kv.second);
        printf("\n");
        break;
    }
    fflush(stdout);
}

// Shape B:宽树 —— 单父 200k 孩子,100 类材料(压分桶/落位/rowOf)
static void scenarioWide() {
    const int NW = 200000, KW = 100;
    auto* wroot = new BenchNode(nullptr, "WROOT");
    for (int i = 0; i < NW; ++i)
        new BenchNode(wroot, "W" + std::to_string(i), "M" + std::to_string(i % KW));
    P.registerRoot(wroot);
    ProjectionSpec spec;
    spec.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* bn = dynamic_cast<BenchNode*>(n);
        return bn ? bn->material() : std::string();
    });
    spec.opt.dynamicAll = true;
    bench("wide-install-200k", NW, [&] {
        P.install(wroot, spec, {});   // 200k 孩子 → 100 桶:bucketByKey find_if 压力
    });
    volatile size_t c = P.childCount(wroot);
    checkOk(c == (size_t)KW, "wide flat size = %zu (expect %d 组)", (size_t)c, KW);
    // rowOf 组内成员。⚠ 基线实测:child(group,i) 每次都 rawChildren 重建
    // 2000 成员向量 + revalidate 校验 owner 的 200k 活列表 → 单次 rowOf
    // = O(组员 × 父孩子数) ≈ 4×10^8 操作,10k 次要跑一小时 → 降到 100 次。
    // 修复(child/childCount 对组 O(1) 直取)后再对比。
    GuiBase* last = wroot->visibleChild(NW - 1);
    bench("wide-rowOf-x100", 100, [&] {
        for (int i = 0; i < 100; ++i) { volatile int r = P.rowOf(last); (void)r; }
    });
    P.unregisterRoot(wroot);
    P.uninstall(wroot);
    delete wroot;   // 连坐政体清场
    P.prune();
}

// ── C4 实证:延迟释放(读侧不释放 = 用内存换安全)的真实代价 ──────────
// ★ 修正一处认知错误(C4 原先把峰值归因于"一次性 wipe 十万组",实测【不成立】):
//   rebuild 通道里组是被【复用】的(matchReusable 按 key 命中),从不 cull,
//   所以那一路 m_retired 全程为 0(实测)。真正会把大量组推进延迟释放队列的
//   是【换规则】—— key 全变 → 复用全失配 → 旧组成片 cull。
//   ⇒ 内存预算该按"换规则/删组那一刻的组数"算,不是按"重建那一刻"。
static void scenarioRegroup() {
    if (!requireBoundShapeA("regroup")) return;   // ★ 同上:没绑定时"旧组成片 cull"必然报 0
    P.resetRetiredHighWater();
    auto mbBefore = P.memoryBreakdown();
    printf("[DIAG] 换规则前: liveGroups=%zu retired=%zu highWater=%zu pages=%zu\n",
           P.liveGroupCount(), mbBefore.retiredGroups, mbBefore.retiredHighWater,
           mbBefore.pages);
    // ★ 必须换【全局绑定】而不是 install(root,...):
    //   10 万个组分佈在 10000 个 P 层节点上,而 bindShapeA 的 locate 只命中
    //   名字以 A/P 开头的节点 —— install(root) 只会重算 root 那一页,
    //   根本碰不到 P 层(实测:旧组一个都没 cull,retired 恒为 0)。
    //   真实的"换分组规则"操作就是改 bindSpec,所以这里也走 bindSpec。
    ProjectionSpec byName;
    byName.grouping = GroupRules::byKey([](GuiBase* n) -> std::string {
        auto* b = dynamic_cast<BenchNode*>(n);
        if (!b) return std::string();
        const std::string& nm = b->name();
        return "N" + (nm.empty() ? std::string("?") : nm.substr(0, 1));
    });
    byName.opt.dynamicAll = true;

    QElapsedTimer t; t.start();
    {
        TreeProjection::SpecBinding b;
        b.id = "bench-material";
        b.locate = [](GuiBase* n) {
            auto* bn = dynamic_cast<BenchNode*>(n);
            return bn && !bn->name().empty() &&
                   (bn->name()[0] == 'A' || bn->name()[0] == 'P');
        };
        b.spec = byName;
        P.bindSpec(std::move(b));   // 同 id 覆盖 → rebind 全树 → 旧组成片 cull
    }
    double ms = (double)t.nsecsElapsed() / 1e6;

    auto mb = P.memoryBreakdown();
    double hwmMB = mb.retiredHighWater * 96.0 / (1024 * 1024);
    double ledgerMB = mb.totalBytes() / (1024.0 * 1024.0);
    // ⚠ printf 格式串里【只用 ASCII 符号】(→ / ≈ / ★ … 会让 MSVC 的 printf
    //   分析器错位 → C4474,并连带 C4819)。详见 CMakeLists 的 /W3 说明。
    printf("[BENCH] %-22s 换规则 %7.1f ms | 旧组成片 cull -> 待回收峰值 %zu 组 ~ %.1f MB"
           " | 新存活组 %zu\n", "regroup-rule-swap", ms, mb.retiredHighWater, hwmMB,
           P.liveGroupCount());
    printf("[BENCH]   -> C4 实证:延迟释放上界 = %zu 组 x %zu B ~ %.1f MB"
           " = 稳定账本 %.1f MB 的 %.1f%%\n",
           mb.retiredHighWater, (size_t)96, hwmMB, ledgerMB,
           ledgerMB > 0 ? hwmMB * 100.0 / ledgerMB : 0.0);

    // 安全点回收(真实调用方就是这样:坐标失效之后才回收)
    t.restart();
    P.collectGarbage();
    double gcMs = (double)t.nsecsElapsed() / 1e6;
    printf("[BENCH] %-22s collectGarbage %6.1f ms | 回收后待回收 %zu | liveGroupCount %zu\n",
           "  └ collectGarbage", gcMs, P.retiredCount(), P.liveGroupCount());
    checkOk(P.retiredCount() == 0, "regroup 后待回收队列已清空 (retired=%zu)", P.retiredCount());

    // 恢复材料规则,让后续场景看到与 build-1M 一致的形态
    P.resetRetiredHighWater();
    bindShapeA();       // 复用原定义:同 id 覆盖 → 全树回到按材料分组
    P.collectGarbage();
checkOk(P.liveGroupCount() == (size_t)NA * NP * 10,
           "规则已恢复为按材料, liveGroupCount = %zu (expect %zu)",
           P.liveGroupCount(), (size_t)NA * NP * 10);
    fflush(stdout);
}

// ══════════════════════════════════════════════════════════════
//  内存安全压测(替代 ASan)—— 把"prune 的内存安全"从 [论证] 升到 [实测]
// ══════════════════════════════════════════════════════════════
// ★ 为什么需要它:PROJECT.md §5 记着一条"[论证] —— collectReachable/prune 与
//   '账页 key 已被数据侧释放'的内存安全,只有可达性论证,没有实证",
//   原因是本机 MSVC AddressSanitizer 运行时无法初始化(进程起不来)。
// ★ 替代方案(不依赖 ASan):两个互补的证据
//   ① Debug CRT:_CrtCheckMemory() 校验**整个 CRT 堆**,任何踩坏(写野指针、
//      双重释放、越界)都会在这里被抓出来 —— 这正是 ASan 能抓的那一类。
//   ② Release:反复建组/删成员/prune/collectGarbage N 轮,看工作集是否
//      单调增长 —— 抓**泄漏**(延迟释放漏回收、僵尸页残留)。
// ⚠ 必须在【每轮之间】查,且轮数够大:泄漏是累积的,单轮看不出来。
static void scenarioGcStress() {
#ifdef _DEBUG
    // _CrtCheckMemory 只在 Debug CRT(/MDd)下有效;Release 下退化为"不可用"
    const bool heapCheck = true;
#else
    const bool heapCheck = false;
#endif
    const int ROUNDS = 200;
    printf("[BENCH] gcstress 开始:%d 轮 | 堆校验(_CrtCheckMemory)=%s\n",
           ROUNDS, heapCheck ? "开(Debug CRT)" : "关(Release CRT 不支持,本轮只查泄漏)");

    // 自包含小树:反复建组 → 删光成员 → 组蒸发进 m_retired → collectGarbage
    auto* root = new BenchNode(nullptr, "GS");
    std::vector<BenchNode*> kids;
    const int K = 200;                       // 每轮 200 个成员 → 10 个组
    ProjectionSpec spec;
    spec.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* b = dynamic_cast<BenchNode*>(n);
        return b ? b->material() : std::string();
    });
    spec.opt.dynamicAll = true;

    double ws0 = workingSetMB();
    size_t retiredLeak = 0, groupLeak = 0;
    int heapFail = 0;
    int roundsPrunePath = 0;          // 走"失控死亡 + prune 剪枝"那条路的轮数
    // ★ 本小树的"基线组数":进入前全引擎有多少组。只断言【结束时回到基线】,
    //   绝不能用 liveGroupCount()==0 做断言 —— 那是【全引擎】计数,本场景
    //   串在 cold/rebuild 后面跑时主树还有 10 万组,会误报 MISMATCH。
    //   (实测踩过:bench all 里 gcstress 报"残留活组 20000000",而 exit=0)
    const size_t groupBaseline = P.liveGroupCount();
    for (int r = 0; r < ROUNDS; ++r) {
        // ★ 两种删除路径轮换 —— 这是本场景存在的意义:
        //   路径 A(偶数轮):删前 discard() 显式讣告 → 账页当场清,组立即 delete。
        //   路径 B(奇数轮):删前【不】discard → 账页 key 变成"已被数据侧释放的野指针",
        //     只能靠 prune 的可达性剪枝清理 —— 而 DFS 途中会拿这些野 key 去
        //     做 map 查找 / 比较。这才是原"[论证] —— collectReachable/prune 与
        //     '账页 key 已被数据侧释放'的内存安全"真正指的路径,必须压到。
        const bool discardPath = (r % 2 == 0);
        kids.clear();
        for (int i = 0; i < K; ++i) {
            auto* b = new BenchNode(root, "G" + std::to_string(i),
                                     "M" + std::to_string(i % 10));
            kids.push_back(b);
        }
        P.install(root, spec, {});
        (void)P.childCount(root);                    // 纯读触发惰性重算(组建成)
        for (BenchNode* b : kids) {
            if (discardPath) P.discard(b);           // 路径 A:显式讣告
            root->removeTreeChild(b);
            delete b;
        }
        P.invalidate(root);                          // ★v4:父页孩子列表变了(两条路径都要;
                                                     //   A/B 验的是"死者账页"的清理路,与此正交)
        (void)P.childCount(root);                    // 脏页重算 → 组蒸发 → m_retired
        if (!discardPath) {
            // 路径 B:账页已成僵尸,靠 prune 清理(这才是要验的那条路)
            P.prune();
            ++roundsPrunePath;
        }
        P.collectGarbage();
        retiredLeak += P.retiredCount();
        // 只数"本小树多出来的组" = 当前全引擎组数 - 进入本场景时的基线
        size_t now = P.liveGroupCount();
        groupLeak += (now > groupBaseline) ? (now - groupBaseline) : 0;
        if (heapCheck && _CrtCheckMemory() != _CRT_ERROR) ++heapFail;
    }
    double ws1 = workingSetMB();
    printf("[BENCH] %-22s %d 轮(含 prune 剪枝路径 %d 轮)| 残留待回收累计=%zu | 本树残留组累计=%zu"
           " | 堆校验失败轮数=%d | WS %.1f->%.1f MB\n",
           "gcstress-x200", ROUNDS, roundsPrunePath, retiredLeak, groupLeak, heapFail, ws0, ws1);
    checkOk(retiredLeak == 0, "每轮回收彻底(残留待回收累计=%zu)", retiredLeak);
checkOk(groupLeak == 0, "每轮本树组全蒸发(残留组累计=%zu,基线 %zu)", groupLeak, groupBaseline);
    // ⚠ 堆校验这一项在 Release 下**不可用**(_CrtCheckMemory 只存在于 Debug CRT)。
    //   必须显式说"跳过",绝不能输出 OK —— 否则就是一个【假绿灯】,
    //   交叉验证的 agent 会误以为 Release 也做了堆校验。
    if (heapCheck)
checkOk(heapFail == 0, "CRT 堆全程无损坏(每轮 _CrtCheckMemory,失败 %d 轮)", heapFail);
    else
        printf("[SKIP ] CRT 堆校验(Release CRT 不支持 _CrtCheckMemory)"
               " → 此项须看 Debug 版或 ASan\n");
    // 泄漏判据:200 轮建/删 4 万个节点,若每轮泄漏任何东西,WS 会明显上跳。
    // 阈值给宽松些(只抓"数量级级别"的泄漏,不是几 MB 的分配器抖动)。
    double growth = ws1 - ws0;
checkOk(growth < 64.0, "无累积泄漏(WS 增长 %.1f MB,阈值 64 MB)", growth);
    P.uninstall(root);
    P.collectGarbage();
    P.unregisterRoot(root);
    delete root;
    fflush(stdout);
}

// prune 单独计时(大树上,含全部账页的可达性 DFS)
static void scenarioPrune() {
    // ★ 最隐蔽的一个:账本空时 prune 无事可做 → 0.0 ns/op,而 exit 仍是 0。
    //   "数字看起来合理"比"明显错"更难发现,所以必须有前置。
    if (!requireBoundShapeA("prune")) return;
    bench("prune-1M", 1010101, [&] { P.prune(); });
}

// prune 的成本归因(常驻诊断,需账本非空 → 与 cold 串跑或用 `bench pbreak`)。
//   A) 纯数据侧全树 DFS          —— 引擎零参与,这是任何剪枝方案的下界
//   B) A + std::unordered_set    —— 再加去重集(= 旧实现):实测 244 ns/节点
//   C) A + PtrSet(开放寻址)     —— 当前实现:实测 50 ns/节点
//   D) 引擎 prune() 实测         —— D-C = 引擎自身(账页查找 + revalidate)
//   B/C/D 三行对照是本工程性能结论的证据链,任何一次改动都能立刻看出谁劣化了。
static void scenarioPruneBreakdown() {
    {   // A:只走虚接口枚举
        QElapsedTimer t; t.start();
        size_t n = 0;
        std::vector<GuiBase*> st{ g_tree.root };
        while (!st.empty()) {
            GuiBase* x = st.back(); st.pop_back();
            if (!x) continue;
            ++n;
            size_t c = x->visibleChildrenCount();
            for (size_t i = 0; i < c; ++i) st.push_back(x->visibleChild(i));
        }
        printf("[AB] A raw-dfs-only        %8.1f ms  (%zu nodes, %.1f ns/node)\n",
               (double)t.nsecsElapsed() / 1e6, n, (double)t.nsecsElapsed() / n);
        fflush(stdout);
    }
    {   // B:同上 + std::unordered_set 去重(prune 记账的必需成本,旧实现)
        QElapsedTimer t; t.start();
        size_t n = 0;
        std::unordered_set<GuiBase*> seen;
        std::vector<GuiBase*> st{ g_tree.root };
        while (!st.empty()) {
            GuiBase* x = st.back(); st.pop_back();
            if (!x || !seen.insert(x).second) continue;
            ++n;
            size_t c = x->visibleChildrenCount();
            for (size_t i = 0; i < c; ++i) st.push_back(x->visibleChild(i));
        }
        printf("[AB] B raw-dfs + unordered_set %6.1f ms  (%zu nodes, %.1f ns/node)\n",
               (double)t.nsecsElapsed() / 1e6, n, (double)t.nsecsElapsed() / n);
        fflush(stdout);
    }
    {   // C:同上 + PtrSet(开放寻址,当前实现用的那个)
        QElapsedTimer t; t.start();
        size_t n = 0;
        PtrSet seen;
        std::vector<GuiBase*> st{ g_tree.root };
        while (!st.empty()) {
            GuiBase* x = st.back(); st.pop_back();
            if (!x || !seen.insert(x)) continue;
            ++n;
            size_t c = x->visibleChildrenCount();
            for (size_t i = 0; i < c; ++i) st.push_back(x->visibleChild(i));
        }
        printf("[AB] C raw-dfs + PtrSet       %6.1f ms  (%zu nodes, %.1f ns/node)\n",
               (double)t.nsecsElapsed() / 1e6, n, (double)t.nsecsElapsed() / n);
        fflush(stdout);
    }
    {   // D:引擎实测(D-C = 引擎自身开销:账页查找 + revalidate + 账本比对)
        QElapsedTimer t; t.start();
        P.prune();
        printf("[AB] D engine prune()         %6.1f ms\n", (double)t.nsecsElapsed() / 1e6);
        fflush(stdout);
    }
    printf("[AB] -> B/C 之差 = 去重集实现的账;D-A = 引擎的全部开销;"
           "D 若已贴近 C,继续压 prune 就是徒劳的\n");
    fflush(stdout);
}

// 反例场景:逐个 install(install 自带 prune → 每次都全树 DFS)
// 只装 20 个,外推 10k 次的代价 —— 暴露"批量必须走 bindSpec/rebind"
static void scenarioInstallLoop() {
    // ★ 本场景的意义是"每个 install 都内嵌一次【全树】prune"的代价,
    //   要外推到 1 万父,前提是树里【已经】有那 1 万个账页。否则只有本场景自己
    //   装的 20 页,外推值毫无意义(实测单独跑 53 ms/install vs 有账本 110 ms/install)。
    if (!requireBoundShapeA("iloop")) return;
    const int K = 20;
    ProjectionSpec spec;
    spec.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* bn = dynamic_cast<BenchNode*>(n);
        return bn ? bn->material() : std::string();
    });
    spec.opt.dynamicAll = true;
    QElapsedTimer t; t.start();
    for (int i = 0; i < K; ++i)
        P.install(g_tree.internals[i], spec, {});   // 重复装同 Spec,幂等但 prune 照跑
    double ms = (double)t.nsecsElapsed() / 1e6;
    printf("[BENCH] %-22s %10.1f ms for %d installs = %.1f ms/install (含全树 prune!)\n",
           "install-loop-x20", ms, K, ms / K);
    printf("[NOTE]  外推:1 万个内部节点逐个 install ~ %.0f s -- 批量场景必须走 bindSpec+rebind\n",
           ms / K * 10000.0 / 1000.0);
    fflush(stdout);
}

// Shape C:深链 100k 级(单独进程跑:递归 DFS/析构的栈深风险)
static int scenarioDeep() {
    const int DEPTH = 100000;
    printf("[DEEP] building chain depth=%d ...\n", DEPTH); fflush(stdout);
    auto* root = new BenchNode(nullptr, "D0");
    BenchNode* cur = root;
    for (int i = 1; i < DEPTH; ++i) {
        cur = new BenchNode(cur, "D" + std::to_string(i));
    }
    P.registerRoot(root);
    printf("[DEEP] prune (collectReachable recursion)...\n"); fflush(stdout);
    QElapsedTimer t; t.start();
    P.prune();
    printf("[DEEP] prune ok %.1f ms\n", (double)t.nsecsElapsed() / 1e6); fflush(stdout);
    printf("[DEEP] childCount walk down...\n"); fflush(stdout);
    t.restart();
    GuiBase* n = root; long long steps = 0;
    while (P.childCount(n) > 0) { n = P.child(n, 0); ++steps; }
    printf("[DEEP] walk ok %lld steps %.1f ms\n", steps, (double)t.nsecsElapsed() / 1e6); fflush(stdout);
    P.unregisterRoot(root);
    // ⚠ 拆链必须【迭代】:~GuiBase 连坐析构是递归的(冻结架构自带属性),
    //   10 万级深链 delete root 会爆 1MB 默认栈 —— 这是数据类自身的深度上限,
    //   与引擎无关(引擎 DFS 已全迭代化)。生产导航树深度通常 <100,无此虞;
    //   真有超深链,要么调 /STACK 链接器储备,要么像这里一样自底向上拆。
    printf("[DEEP] dismantling chain iteratively (~GuiBase recursion is architecture-bound)...\n");
    fflush(stdout);
    t.restart();
    std::vector<BenchNode*> path;
    for (BenchNode* x = root; x; x = x->visibleChildrenCount() ?
             static_cast<BenchNode*>(x->visibleChild(0)) : nullptr)
        path.push_back(x);
    for (size_t i = path.size(); i-- > 0;) {
        if (i > 0) path[i - 1]->removeTreeChild(path[i]);   // 先摘除,断连坐
        delete path[i];                                     // 无孩子 → 析构不递归
    }
    printf("[DEEP] dismantle ok %.1f ms\n", (double)t.nsecsElapsed() / 1e6); fflush(stdout);
    printf("[DEEP] PASS: depth %d 引擎全链路(建树/prune/下行)无栈溢出\n", DEPTH);
    printf("[DEEP] 拆链后残留组 = %zu(期望 0)\n", P.liveGroupCount());
    checkOk(P.liveGroupCount() == 0, "deep 拆链后零残留组");
    return g_checkFail ? 1 : 0;
}

// ── dbg:微型复现"rebuild 后组数减半"(2A × 2P × 4F,2 种材料)──
static int scenarioDbg() {
    // 小树:每个 P 挂 4 叶子、2 种材料 → 期望每 P 2 组、共 4 组
    auto* root = new BenchNode(nullptr, "ROOT");
    std::vector<BenchNode*> parts;
    for (int a = 0; a < 2; ++a) {
        auto* an = new BenchNode(root, "A" + std::to_string(a));
        for (int p = 0; p < 2; ++p) {
            auto* pn = new BenchNode(an, "P" + std::to_string(a * 2 + p));
            parts.push_back(pn);
            for (int f = 0; f < 4; ++f)
                new BenchNode(pn, "F" + std::to_string(f), MATERIALS[f % 2]);
        }
    }
    P.registerRoot(root);
    TreeProjection::SpecBinding b;
    b.id = "dbg-material";
    b.locate = [](GuiBase* n) {
        auto* bn = dynamic_cast<BenchNode*>(n);
        return bn && !bn->name().empty() && (bn->name()[0] == 'A' || bn->name()[0] == 'P');
    };
    b.spec.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* bn = dynamic_cast<BenchNode*>(n);
        return bn ? bn->material() : std::string();
    });
    b.spec.opt.dynamicAll = true;
    P.bindSpec(std::move(b));

    auto dump = [&](const char* phase) {
        printf("[DBG] %s: liveGroupCount=%zu | ", phase, P.liveGroupCount());
        for (size_t i = 0; i < parts.size(); ++i)
            printf("P%zu(childCount=%zu,hasProj=%d) ", i,
                   P.childCount(parts[i]), (int)P.hasProjection(parts[i]));
        printf("\n");
        fflush(stdout);
    };
    dump("after-bind");

    // wipe:记录旧叶子地址,验证地址复用
    std::vector<GuiBase*> oldAddrs;
    for (BenchNode* pn : parts)
        for (size_t i = 0; i < pn->visibleChildrenCount(); ++i)
            oldAddrs.push_back(pn->visibleChild(i));
    for (BenchNode* pn : parts) {
        while (pn->visibleChildrenCount() > 0) {
            GuiBase* c = pn->visibleChild(0);
            P.notifyRemove(pn, c);                   // ★v4 通知(delete 前)
            pn->removeTreeChild(c);
            delete c;
        }
    }
    dump("after-wipe");

    // rebuild:同形状,记录新地址与旧地址的复用率
    size_t reused = 0;
    std::vector<GuiBase*> newAddrs;
    for (BenchNode* pn : parts) {
        for (int f = 0; f < 4; ++f) {
            auto* nf = new BenchNode(pn, "F" + std::to_string(f), MATERIALS[f % 2]);
            newAddrs.push_back(nf);
        }
        P.invalidate(pn);                            // ★v4:重建后通知(双配置输出确定)
    }
    for (GuiBase* n : newAddrs)
        for (GuiBase* o : oldAddrs)
            if (n == o) { ++reused; break; }
    printf("[DBG] address reuse: %zu/%zu new leaves sit on old addresses\n",
           reused, newAddrs.size());
    dump("after-rebuild(pre-rebind)");

    P.rebind();
    dump("after-rebind");

    // 逐 P 检查组数与成员
    for (size_t i = 0; i < parts.size(); ++i) {
        printf("[DBG] P%zu flat:", i);
        size_t c = P.childCount(parts[i]);
        for (size_t k = 0; k < c; ++k) {
            GuiBase* x = P.child(parts[i], k);
            if (auto* g = dynamic_cast<TreeGroup*>(x))
                printf(" [%s:%zu]", g->ticket().key.c_str(), P.childCount(g));
            else
                printf(" %s", dynamic_cast<BenchNode*>(x)->name().c_str());
        }
        printf("\n");
    }
    fflush(stdout);

    P.unregisterRoot(root);
    delete root;
    // ★ 清僵尸必须挂一个临时探针根 —— m_roots 为空时 prune 直接 return(设计如此),
    //   直接调 prune 等于什么都没做。main 场景一直守着这条,本场景原先漏了,
    //   被 `return 0` 掩盖成"零泄漏"(CHECK 机制上线后才暴露,见 g_checkFail 注释)。
    {
        BenchNode probeDbg(nullptr, "__purge_dbg__");
        P.registerRoot(&probeDbg);
        P.prune();
        P.unregisterRoot(&probeDbg);
    }
    printf("[DBG] teardown liveGroupCount=%zu\n", P.liveGroupCount());
    checkOk(P.liveGroupCount() == 0, "dbg 拆解后零残留组 (liveGroupCount=%zu)", P.liveGroupCount());
    return g_checkFail ? 1 : 0;
}

// ── main ─────────────────────────────────────────────
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    std::string arg = argc > 1 ? argv[1] : "all";

    if (arg == "deep") return scenarioDeep();
    if (arg == "dbg")  return scenarioDbg();

    // 逗号分隔的场景串(二分定位污染用):如 "cold,rebuild"
    auto wants = [&](const char* name) {
        if (arg == "all") return true;
        return arg.find(name) != std::string::npos;
    };

    printf("== TreeProjection bench · Release 数据才有意义 · WS 起点 %.0f MB ==\n",
           workingSetMB());

    // 只有单跑 wide(自带树)时才不建 Shape A;all/其它场景都要先建树
    if (arg == "all" || arg.find("wide") == std::string::npos) buildShapeA();

    // pbreak 要在"账本非空"状态下测 prune(账本空时 prune 直接 return,测不出东西)
    bool wantCold = wants("cold") || wants("pbreak");
    if (wantCold)        scenarioCold();
    else { P.registerRoot(g_tree.root); }   // 后续场景至少需要根注册
    if (wants("hot"))     scenarioHot();
    if (wants("query"))   scenarioQuery();
    if (wants("model"))   scenarioModel();
    if (wants("incr"))    scenarioIncremental();
    if (wants("rebuild")) scenarioRebuild();
    if (wants("wide"))    scenarioWide();
    if (wants("prune"))   scenarioPrune();
    if (wants("pbreak"))  scenarioPruneBreakdown();
    if (wants("iloop"))   scenarioInstallLoop();
    if (wants("regroup")) scenarioRegroup();
    if (wants("gcstress")) scenarioGcStress();
    if (wants("mem"))     scenarioMemAndRates();

    // 汇总
    printf("\n======== 汇总(Release) ========\n");
    double total = 0;
    for (auto& r : g_results) { total += r.ms; }
    for (auto& r : g_results)
        printf("  %-24s %10.1f ms\n", r.name, r.ms);
    printf("  %-24s %10.1f ms\n", "TOTAL", total);
    printf("  峰值工作集 ~ %.0f MB\n", workingSetMB());

    // 清场:引擎账本清零自检。
    // ⚠ prune 在 m_roots 为空时直接 return(设计如此)——清僵尸要挂一个
    //   临时探针根再 prune,可达集只剩它自己,全部僵尸账页被清掉。
    P.unregisterRoot(g_tree.root);
    delete g_tree.root;
    {
        BenchNode probe(nullptr, "__purge__");
        P.registerRoot(&probe);
        P.prune();
        P.unregisterRoot(&probe);
    }
checkOk(P.liveGroupCount() == 0, "teardown liveGroupCount = %zu (expect 0)", P.liveGroupCount());

    // ★ 退出码 = CHECK 失败数(不是恒 0)。
    //   原先这里写死 return 0,于是"屏幕上打了 MISMATCH、exit code 仍是 0",
    //   自动化回归完全抓不到 —— 比没有检查更危险。详见 g_checkFail 的注释。
    printf("\n════ CHECK 失败数 = %d ════\n", g_checkFail);
    fflush(stdout);
    return g_checkFail ? 1 : 0;
}

