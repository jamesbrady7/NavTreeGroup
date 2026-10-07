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
#include "navtreemodel.h"   // ★ P4 需要 NavTreeModel + QPersistentModelIndex
                            //   (QAbstractItemModel 在 QtCore 里,本目标只链 Core 也够)

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

// ── P4:退役组被回收之后,视图可能仍持有它的 persistent index ──────────
//  C1 的防线是这样的:组的淘汰可能发生在【读】里(childCount == rowCount,
//  模型每次展开都会调),那一刻没有任何 begin/end 括号、引擎也没资格发信号
//  ⇒ 当场 delete 就是 UAF ⇒ 于是只入 m_retired,等某个括号的 end 之后释放。
//
//  本探针检验这条防线在【"通知了、但没开括号" + "事后一个不相干的括号"】这条
//  路径上是否仍然成立。注意这不是"漏通知"违约:`notifyRemove` 本身不要求括号
//  (§1.1 的调用前提是"其余无"),守约调用方完全可能这么写。
//
//  判据(安全,【不】解引用):complete 一次释放后 held.isValid() 是否仍为 true。
//  仍为 true ⇒ Qt 从没被告知过这个组没了,而它持有的 internalPointer 已经是
//  被 delete 掉的对象 ⇒ 之后任何一次 rowCount(held)/展开态检查都是 UAF。
static void probe4() {
    printf("== P4 retired group vs view-held persistent index ==\n");
    auto* root = new ProbeNode(nullptr, "R4");
    auto* a    = new ProbeNode(root, "A", "Steel");
    new ProbeNode(root, "B", "Alum");
    new ProbeNode(root, "C", "Copper");
    auto* d    = new ProbeNode(root, "D", "Copper");   // Copper 有两个成员:删一个不蒸发
    P.registerRoot(root);
    P.install(root, matSpec(), {});

    NavTreeModel model(root, P);
    model.setLabeler([](GuiBase* x) { return QString::fromStdString(x ? x->type() : "?"); });

    // 视图"抱着"Steel 组 —— 等价于 QTreeView 把展开过的组放进 expandedIndexes
    // (这正是 C1 自己写的前提:"视图对每个展开的组持有 QPersistentModelIndex")。
    GuiBase* steelGroup = nullptr;
    for (size_t i = 0; i < P.childCount(root); ++i) {
        auto* g = dynamic_cast<TreeGroup*>(P.child(root, i));
        if (g && g->ticket().key == "Steel") { steelGroup = g; break; }
    }
    if (!steelGroup) {
        printf("  [SKIP] P4: Steel 组没建出来(前置不成立,本次测量无效)\n");
        P.uninstall(root); P.unregisterRoot(root); P.collectGarbage(); delete root;
        return;
    }
    QPersistentModelIndex held(model.indexFor(steelGroup));
    printf("  held: Steel group @row %d, isValid=%d\n", held.row(), (int)held.isValid());

    // ① 守约通知,但【不开任何括号】:Steel 组唯一的成员 A 没了
    P.notifyRemove(root, a);
    root->removeTreeChild(a);
    delete a;
    (void)model.rowCount(QModelIndex());          // 纯读 → 重算 → Steel 组蒸发 → 入 m_retired
    const size_t retiredAfterEvaporate = P.retiredCount();
    printf("  after bracket-less notifyRemove + read: retired=%zu, held.isValid=%d\n",
           retiredAfterEvaporate, (int)held.isValid());

    // 前提自检:① 确实发生过蒸发(入队了);② 入队时 Qt 仍认为 held 有效 ——
    //   后者是 C1 的立论前提("视图抱着它、而引擎不知道")。两条缺一 = 本次测量无效。
    if (retiredAfterEvaporate == 0 || !held.isValid()) {
        printf("  [SKIP] P4: 前提不成立(retired=%zu / held.isValid=%d) —— 本次测量无效\n",
               retiredAfterEvaporate, (int)held.isValid());
        P.uninstall(root); P.unregisterRoot(root); P.collectGarbage(); delete root;
        return;
    }

    // ② 一次【不相干】的精准删除(Copper 还剩 C,不蒸发),走标准 removeRows 括号。
    //    ★ 期望:更早那个"无信号覆盖"的残留【不得】被释放 —— 队列水位停在 1。
    //    修复前这里会 drop 到 0(整队列被兑现)→ 组被 delete 而 held 仍有效 → UAF。
    GuiBase* copperGroup = P.parentOf(d);
    const int dRow = P.rowOf(d);
    model.beginRowRemoval(copperGroup, dRow);
    P.notifyRemove(root, d);
    root->removeTreeChild(d);
    delete d;
    model.endRowRemoval();
    const size_t retiredAfterRowChannel = P.retiredCount();
    printf("  after unrelated removeRows window: retired=%zu(期望仍为 %zu:更早的残留必须留着), held.isValid=%d\n",
           retiredAfterRowChannel, retiredAfterEvaporate, (int)held.isValid());
    const bool rowChannelSafe = (retiredAfterRowChannel == retiredAfterEvaporate);
    if (!rowChannelSafe) {
        printf("  [CONFIRMED] P4a: removeRows 通道释放了【未被本次信号覆盖】的残留组\n");
        printf("                 (它已 delete,而 held 仍有效 ⇒ internalPointer 悬空)\n");
        ++g_confirmed;
    } else {
        printf("  [OK] P4a: removeRows 通道只回收本窗口新退役的,更早残留留给 reset 通道\n");
    }

    // ③ 换 reset 通道兑现整队列 —— 这条路的判据要两条同时成立:
    //    「队列确实清空(retired==0,组真的被释放了)」且「Qt 已作废该 index」。
    //    两者同时成立才叫"既释放又不悬空";只满足前一半 = 复盘 ② 的洞。
    model.beginFullReset();                       // 只为拿一个"作废全部坐标"的括号
    model.endFullReset();                         // endResetModel(全部作废) + collectGarbage(整队列)
    const size_t retiredAfterReset = P.retiredCount();
    printf("  after reset channel: retired=%zu(期望 0), held.isValid=%d(期望 0)\n",
           retiredAfterReset, (int)held.isValid());
    if (retiredAfterReset != 0) {
        printf("  [CONFIRMED] P4b: reset 通道没有清空队列\n");
        ++g_confirmed;
    } else if (held.isValid()) {
        printf("  [CONFIRMED] P4b: 组已释放而 Qt 仍认为该 index 有效(internalPointer 悬空)\n");
        ++g_confirmed;
    } else {
        printf("  [OK] P4b: reset 通道既释放了组、又已作废其坐标(释放点与坐标失效同窗)\n");
    }

    P.uninstall(root);
    P.unregisterRoot(root);
    P.collectGarbage();
    delete root;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    probe1();
    probe2();
    probe3();
    probe4();
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
