// ═══════════════════════════════════════════════════════════════
//  probe_review.cpp —— 评审方独立探针(v4 版,PROJECT.md §6 验证方职责)
// ═══════════════════════════════════════════════════════════════
//  v3 评审轮的三个疑点(R1 纪元缺口 / R2 m_nodePage 泄漏 / R3 组上求值 locate)
//  已随 v4 重构改变了形态,本探针按【v4 契约】重新钉死三件事:
//
//  P1 v4 通知契约的两半:
//     a) notifyRemove 之后,裸 rowOf/parentOf(不经任何 childCount 预热)必须
//        立即新鲜 —— 这是 v3 R1 事故(快路径吃陈旧账本)的正面回归;
//     b) 静默删除(违约漏通知)→ 允许陈旧,但 prune 卫生边界必须自愈 ——
//        这是"忘了通知最多陈旧到下一次 prune"承诺的兑现验证。
//  P2 位置索引 entries 随页同生共死:uninstall 后本页 6 条(2 组+4 成员)全部消失
//     (v4 单表在页内,无全局副本 → v3 R2 的清理不对称在结构上不可能)。
//  P3 walkBindings 永不对组节点求值 locate(不变量#3,v3 R3 的回归)。
//
//  退出码 = 坐实的违约数(0 = 契约全部成立)。
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

// ── P1a:通知即新鲜(裸查询,无预热)── / ── P1b:漏通知 → 陈旧至 prune ──
static void probe1() {
    printf("== P1 v4 notification contract ==\n");
    auto* root = new ProbeNode(nullptr, "R1");
    auto* pg   = new ProbeNode(root, "PG");
    auto* l1   = new ProbeNode(pg, "L1", "A");
    new ProbeNode(pg, "L2", "B");
    auto* l3   = new ProbeNode(pg, "L3", "A");
    new ProbeNode(pg, "L4", "B");
    P.registerRoot(root);
    P.install(pg, matSpec(), {});          // GA=[L1,L3]  GB=[L2,L4]

    (void)P.childCount(pg);                // 建账:GA 里 L3 行号 = 1
    int r0 = P.rowOf(l3);
    printf("  baseline rowOf(L3)=%d (expect 1)\n", r0);

    // ── a) 守约:notifyRemove → 裸 rowOf 必须立即 0 ──
    P.notifyRemove(pg, l1);
    pg->removeTreeChild(l1);
    delete l1;
    int rA = P.rowOf(l3);                  // 裸查询,不经 childCount
    if (rA != 0) {
        printf("  [CONFIRMED] P1a: after notifyRemove bare rowOf(L3)=%d (truth 0)\n", rA);
        ++g_confirmed;
    } else {
        printf("  [OK] P1a: notifyRemove -> bare rowOf fresh (0)\n");
    }

    // ── b) 违约:静默删 L2 → 允许陈旧;prune 必须自愈 ──
    auto* l2p = pg->visibleChild(0);       // 现在的原始孩子 = [L2,L3,L4] 里找 L2
    // 直接按名字找,避免依赖顺序:
    for (size_t i = 0; i < pg->visibleChildrenCount(); ++i) {
        auto* bn = dynamic_cast<ProbeNode*>(pg->visibleChild(i));
        if (bn && bn->name() == "L2") { l2p = bn; break; }
    }
    pg->removeTreeChild(l2p);
    delete l2p;                            // 零通知(违约)
    int staleRows = (int)P.childCount(pg->visibleChild(0));  // 任意纯读:GB 可能仍显示 2 员(陈旧,契约允许)
    (void)staleRows;
    P.prune();                             // 卫生边界:必须自愈
    // 自愈后:GB 只剩 L4。找到 GB 验证成员数:
    int gbMembers = -1;
    for (size_t i = 0; i < P.childCount(pg); ++i) {
        auto* g = dynamic_cast<TreeGroup*>(P.child(pg, i));
        if (g && g->ticket().key == "B") gbMembers = (int)P.childCount(g);
    }
    if (gbMembers != 1) {
        printf("  [CONFIRMED] P1b: after silent delete + prune, GB members=%d (truth 1)\n", gbMembers);
        ++g_confirmed;
    } else {
        printf("  [OK] P1b: silent delete stale-tolerated, prune healed (GB=1)\n");
    }

    P.unregisterRoot(root);
    P.uninstall(pg);
    P.collectGarbage();
    delete root;
}

// ── P2:entries 随页同生共死 ──
static void probe2() {
    printf("== P2 entries lifecycle ==\n");
    auto* root = new ProbeNode(nullptr, "R2");
    auto* pg   = new ProbeNode(root, "PG");
    for (int i = 0; i < 4; ++i)
        new ProbeNode(pg, "L" + std::to_string(i), i % 2 ? "B" : "A");
    P.registerRoot(root);
    P.install(pg, matSpec(), {});
    (void)P.childCount(pg);                // 建账:flat 2 组 + 4 成员 = entries 6 条
    size_t np_before = P.memoryBreakdown().entries;
    P.uninstall(pg);
    P.collectGarbage();
    size_t np_after = P.memoryBreakdown().entries;
    printf("  entries: before uninstall=%zu after=%zu (page contributed 6)\n", np_before, np_after);
    if (np_before - np_after != 6) {
        printf("  [CONFIRMED] P2: removed %zu of 6 entries\n", np_before - np_after);
        ++g_confirmed;
    } else {
        printf("  [OK] P2: all 6 entries died with the page\n");
    }
    P.unregisterRoot(root);
    delete root;
}

// ── P3:rebind 期间 locate 不得被叫到组节点上 ──
static int g_locateOnGroups = 0;
static void probe3() {
    printf("== P3 locate never evaluated on groups ==\n");
    auto* root = new ProbeNode(nullptr, "R3");
    auto* pg   = new ProbeNode(root, "PG");
    for (int i = 0; i < 4; ++i)
        new ProbeNode(pg, "L" + std::to_string(i), i % 2 ? "B" : "A");
    P.registerRoot(root);
    P.install(pg, matSpec(), {});          // 先建出组,DFS 才会走到组节点
    (void)P.childCount(pg);
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
        printf("  [CONFIRMED] P3: invariant-3 violated (locate saw groups)\n");
        ++g_confirmed;
    } else {
        printf("  [OK] P3: locate never saw a group node\n");
    }
    P.unbindSpec("probe3");
    P.unregisterRoot(root);
    P.uninstall(pg);
    P.collectGarbage();
    delete root;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    probe1();
    probe2();
    probe3();
    printf("== summary: %d confirmed ==\n", g_confirmed);
    {   // 清场:挂探针根再 prune(m_roots 为空时 prune 直接 return,清不掉僵尸)
        ProbeNode probe(nullptr, "__purge__");
        P.registerRoot(&probe);
        P.prune();
        P.unregisterRoot(&probe);
    }
    P.collectGarbage();
    printf("teardown: liveGroups=%zu retired=%zu\n", P.liveGroupCount(), P.retiredCount());
    return g_confirmed ? 1 : 0;
}
