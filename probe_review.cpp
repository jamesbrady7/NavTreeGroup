// ═══════════════════════════════════════════════════════════════
//  probe_review.cpp —— 评审方独立探针(2026-10-06,PROJECT.md §6 验证方职责)
// ═══════════════════════════════════════════════════════════════
//  目的:对读码发现的三个疑点做【可复跑实证】,不猜测、不以读码代替实测。
//
//  P1 纪元缺口(高危嫌疑):parentOf/rowOf 的快路径在 EpochScope 之前执行,
//     裸调用它们不会递增 m_queryEpoch → 内部 revalidate 可能命中
//     "checkedAt == m_queryEpoch" 闸门,吃到【上一次操作】的陈旧账本。
//     头文件承诺"跨操作纪元必递增 → 一定重新枚举"(m_queryEpoch 注释),
//     若实证成立,则快路径违反了该承诺(旧实现无快路径时不存在此窗口)。
//  P2 m_nodePage 清理漏项(中危嫌疑):dropProjection 的清理循环收集
//     p.flat + p.members 的【键(组指针)】,但 memberToPos/m_nodePage 的键集
//     是 flat ∪ 各组成员(kv.second)→ 被分组的数据节点条目不会被清掉。
//  P3 不变量#3 字面漂移(低危):walkBindings 的条件是
//     `b.locate(n) && (maybeGroup ? !isGroup(n) : true)` —— locate 会先对
//     组节点求值(旧实现 `!isGroup(n) && b.locate(n)` 从不求值)。
//     探针只统计"locate 被叫到组节点的次数",>0 即漂移坐实。
//
//  退出码 = 坐实的疑点数(0 = 三个疑点全部不成立)。
#include <cstdio>
#include <string>
#include <vector>
#include "guibase.h"
#include "treeprojection.h"

static TreeProjection& P = TreeProjection::instance();
static int g_confirmed = 0;

class ProbeNode : public GuiBase {
public:
    ProbeNode(GuiBase* p, std::string name, std::string mat = {})
        : GuiBase(p), m_name(std::move(name)), m_mat(std::move(mat)) {
        if (p) p->addTreeChild(this);
    }
    std::string type() override { return "Probe"; }
    const std::string& name() const { return m_name; }
    const std::string& material() const { return m_mat; }
private:
    std::string m_name, m_mat;
};

static ProjectionSpec matSpec() {
    ProjectionSpec s;
    s.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* bn = dynamic_cast<ProbeNode*>(n);
        return bn ? bn->material() : std::string();
    });
    s.opt.dynamicAll = true;
    return s;
}

// ── P1:数据侧静默变更后,裸 rowOf 快路径是否吃陈旧账本 ──
static void probe1() {
    printf("== P1 epoch fast-path staleness ==\n");
    auto* root = new ProbeNode(nullptr, "R1");
    auto* pg   = new ProbeNode(root, "PG");
    auto* l1   = new ProbeNode(pg, "L1", "A");
    new ProbeNode(pg, "L2", "B");
    auto* l3   = new ProbeNode(pg, "L3", "A");
    new ProbeNode(pg, "L4", "B");
    P.registerRoot(root);
    P.install(pg, matSpec(), {});          // GA=[L1,L3]  GB=[L2,L4]

    // op1:一次正常校验(开纪元、建位置索引)。L3 在 GA 里的行号 = 1
    size_t c1 = P.childCount(pg);
    int r_before = P.rowOf(l3);
    printf("  op1 childCount=%zu rowOf(L3)=%d (expect 1)\n", c1, r_before);

    // 数据侧静默删除 L1(引擎不感知 —— 这正是"惰性发现"设计要处理的常态)
    pg->removeTreeChild(l1);
    delete l1;

    // op2:【裸】rowOf —— 旧语义应重新枚举活列表,给出真值 0(GA=[L3]);
    //      若快路径吃了 op1 纪元的陈旧账本,会答 1
    int r_stale = P.rowOf(l3);

    // op3:childCount 开新纪元强制重算后,再问一次拿真值
    size_t c2 = P.childCount(pg);
    int r_true = P.rowOf(l3);
    printf("  after silent delete of L1: bare rowOf(L3)=%d | after childCount: rowOf(L3)=%d (truth 0), childCount=%zu\n",
           r_stale, r_true, c2);

    if (r_stale != r_true) {
        printf("  [CONFIRMED] P1: bare rowOf returned stale answer %d (truth %d)\n", r_stale, r_true);
        ++g_confirmed;
    } else {
        printf("  [OK] P1: bare rowOf fresh (hole not reproducible this way)\n");
    }

    P.unregisterRoot(root);
    P.uninstall(pg);
    delete root;
}

// ── P2:uninstall 后 m_nodePage 是否残留被分组成员的条目 ──
static void probe2() {
    printf("== P2 m_nodePage cleanup on dropProjection ==\n");
    auto* root = new ProbeNode(nullptr, "R2");
    auto* pg   = new ProbeNode(root, "PG");
    for (int i = 0; i < 4; ++i)
        new ProbeNode(pg, "L" + std::to_string(i), i % 2 ? "B" : "A");
    P.registerRoot(root);
    P.install(pg, matSpec(), {});
    size_t c = P.childCount(pg);           // 建索引:flat 2 组 + 4 成员 = 6 条
    size_t np_before = P.memoryBreakdown().nodePage;
    size_t mtp_hint = 0;                   // 本页 memberToPos 规模推断:6
    (void)c; (void)mtp_hint;
    P.uninstall(pg);                        // dropProjection(deferDelete=false)
    P.collectGarbage();                     // 清掉立即删除语义下可能的残留(不影响 m_nodePage)
    size_t np_after = P.memoryBreakdown().nodePage;
    printf("  nodePage entries: before uninstall=%zu after=%zu (page contributed 6: 2 groups + 4 members)\n",
           np_before, np_after);
    size_t removed = np_before - np_after;
    if (removed < 6) {
        printf("  [CONFIRMED] P2: only %zu of 6 entries removed -> %zu stale entries left\n",
               removed, 6 - removed);
        ++g_confirmed;
    } else {
        printf("  [OK] P2: all 6 entries removed\n");
    }
    P.unregisterRoot(root);
    delete root;
}

// ── P3:rebind 期间 locate 是否被叫到组节点上 ──
static int g_locateOnGroups = 0;
static void probe3() {
    printf("== P3 locate evaluated on group nodes ==\n");
    auto* root = new ProbeNode(nullptr, "R3");
    auto* pg   = new ProbeNode(root, "PG");
    for (int i = 0; i < 4; ++i)
        new ProbeNode(pg, "L" + std::to_string(i), i % 2 ? "B" : "A");
    P.registerRoot(root);
    P.install(pg, matSpec(), {});           // ★ 先手动装页建出组,否则树里没有组节点,
                                            //   walkBindings 根本走不到组 → 探针空转(第一版设计缺陷)
    P.childCount(pg);                       // 确保 flat/组已生成
    TreeProjection::SpecBinding b;
    b.id = "probe3";
    b.locate = [](GuiBase* n) {
        if (P.isGroup(n)) ++g_locateOnGroups;   // 只计数,永不命中
        return false;
    };
    P.bindSpec(std::move(b));
    P.rebind();
    printf("  locate() calls on TreeGroup nodes during bindSpec+rebind: %d\n", g_locateOnGroups);
    if (g_locateOnGroups > 0) {
        printf("  [CONFIRMED] P3: invariant-3 wording ('locate never evaluated on groups') drifted\n");
        ++g_confirmed;
    } else {
        printf("  [OK] P3: locate never saw a group node\n");
    }
    P.unbindSpec("probe3");
    P.unregisterRoot(root);
    P.uninstall(pg);   // 幂等(可能没装过)
    delete root;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    probe1();
    probe2();
    probe3();
    printf("== summary: %d confirmed ==\n", g_confirmed);
    // 清场自检
    {
        ProbeNode probe(nullptr, "__purge__");
        P.registerRoot(&probe);
        P.prune();
        P.unregisterRoot(&probe);
    }
    printf("teardown: liveGroups=%zu retired=%zu\n", P.liveGroupCount(), P.retiredCount());
    return 0;   // 探针只报告,不设失败退出码(结论由评审文本给出)
}
