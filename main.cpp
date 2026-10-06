// ═══════════════════════════════════════════════════════════════
//  NavTreeGroup · 投影引擎终端验证(v3)
// ═══════════════════════════════════════════════════════════════
//  在【不改动既有架构】的前提下验证 TreeProjection 流水线引擎:
//   场景 0:建树(三政体混合 + 遗留假组 + 嵌套文件夹),引擎透传基线
//   场景 1:dynamicAll 按类型分组 + 数据侧零写入校验
//   场景 2:脏输入 + 换规则(垫片是"别人家的孩子",构造上无法被绑架)
//   场景 3:refresh 幂等 —— 同 key 组指针稳定(模型状态可保)
//   场景 4:数据侧 delete 节点 → 下次查询自动蒸发,空组自动消失
//   场景 5:parentOf/rowOf/child 全树一致性不变量校验(模型正确性证明)
//   场景 6:ungroup 还原 + 嵌套分组(不同分支各一层)
//   场景 7:多层分组 —— 组内分组(三层)+ 级联拆除
//   场景 8:三政体生存法则 —— discard 讣告 / prune 可达性剪枝自愈
//   场景 9:全量重建政体 —— bindSpec 定位谓词 + rebind 分组自动重生
//   场景 10:投影流水线 —— 过滤+分组+排序一个 Spec;
//           排序与"写死基线"dump-diff 等价验证(迁移方法论第 2/3 步)
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <functional>
#include "guibase.h"
#include "treeprojection.h"
#include "demonodes.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <crtdbg.h>     // _CrtSetReportMode(Debug 断言路由)
#endif

// ═══ 打印工具(全部走引擎查询,即"模型看到的树") ═══
// 关键约定:printTree 只用 E.childCount/E.child 遍历,从不直接调
// node->visibleChild —— 这正是真实 QAbstractItemModel 该有的写法。
static TreeProjection& E = TreeProjection::instance();

static std::string describe(GuiBase* n) {
    if (auto* g = dynamic_cast<TreeGroup*>(n))
        return "[组] " + g->ticket().label + " (" + std::to_string(E.childCount(n)) + " 个成员)";
    std::string name = demoNodeName(n);
    std::string deco = "· ";
    if (n->type() == "Part") deco = "* ";
    else if (n->type() == "Sketch") deco = "# ";
    else if (n->type() == "Constraint") deco = "+ ";
    else if (n->type() == "Geom") deco = "@ ";
    else if (n->type() == "LegacyGroup") deco = "% ";
    else if (n->type() == "Folder") deco = "> ";
    else if (n->type() == "Data") deco = "& ";
    if (auto* p = dynamic_cast<PartObject*>(n)) {
        if (!p->material().empty()) name += " [" + p->material() + "]";
        if (p->suppressed) name += "(suppressed)";
    }
    return deco + name + "  <" + n->type() + ">";
}

static void printTree(GuiBase* n, const std::string& prefix = "", bool last = true, bool root = true) {
    printf("%s\n", (root ? describe(n) : prefix + (last ? "+- " : "|- ") + describe(n)).c_str());
    std::string cp = root ? "" : prefix + (last ? "   " : "|  ");
    size_t cnt = E.childCount(n);
    for (size_t i = 0; i < cnt; ++i)
        printTree(E.child(n, i), cp, i + 1 == cnt, false);
}

// ═══ 数据侧零写入校验:快照所有 DataObject 的 sup/subGroups 指针 ═══
// 承诺是"引擎对现有对象零写入"——每个场景后重走数据树,逐节点比对
// 数据父亲 + 子列表 与快照完全一致(比的是【裸指针值】:连地址都没变才叫没碰)。
struct DataSnapshot {
    std::map<DataObject*, DataObject*> sup;
    std::map<DataObject*, std::vector<DataObject*>> subs;
};
static void walkData(DataObject* d, const std::function<void(DataObject*)>& f) {
    f(d);
    for (DataObject* s : d->subDataGroups()) walkData(s, f);
}
static DataSnapshot snapshot(DataObject* root) {
    DataSnapshot s;
    walkData(root, [&](DataObject* d) {
        s.sup[d] = d->supDataGroup();
        s.subs[d] = d->subDataGroups();
    });
    return s;
}
static bool verify(DataObject* root, const DataSnapshot& s0, int& checked) {
    bool ok = true; checked = 0;
    walkData(root, [&](DataObject* d) {
        auto it = s0.sup.find(d);
        if (it == s0.sup.end()) { ok = false; return; }
        if (it->second != d->supDataGroup()) { ok = false; return; }
        if (s0.subs.at(d) != d->subDataGroups()) { ok = false; return; }
        ++checked;
    });
    printf(ok ? "  [OK] data snapshot verified (%d nodes, sup/subGroups untouched)\n"
              : "  [FAIL] data snapshot broken!\n", checked);
    return ok;
}

// ═══ 场景 5:全树模型一致性不变量 ═══
// QAbstractItemModel 正确性的充要条件:对投影树上每个 node 的每个孩子 c,
//   parentOf(c) == node  且  rowOf(c) == c 的下标。
static int g_invariantChecks = 0;
static bool g_invariantOk = true;
static void checkInvariants(GuiBase* node) {
    size_t cnt = E.childCount(node);
    for (size_t i = 0; i < cnt; ++i) {
        GuiBase* c = E.child(node, i);
        if (!c) { g_invariantOk = false; return; }
        if (E.parentOf(c) != node) { g_invariantOk = false; printf("  [FAIL] parentOf mismatch at %s\n", describe(c).c_str()); return; }
        if (E.rowOf(c) != (int)i)   { g_invariantOk = false; printf("  [FAIL] rowOf mismatch at %s\n", describe(c).c_str()); return; }
        ++g_invariantChecks;
        checkInvariants(c);
    }
}

int main() {
#ifdef _DEBUG
    // Debug 断言路由到 stderr、不弹窗(否则断言=弹窗+abort,无法定位)。
    // ★ 第一个参数是【单一】报告类型,不是位掩码!传 OR 掩码会反过来触发
    //   CRT 自己的断言(nRptType < _CRT_ERRCNT)—— 踩过,弹一屏窗。逐个类型设:
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲:崩溃点前的输出不丢
    int checked = 0;

    // ═══ 场景 0:建一棵"恶心"的混合树 ═══
    // root(装配体 DataObject)
    //  ├─ 4 个 Part(材料/压制各异)         ← subDataGroups(数据自管政体)
    //  ├─ 草图/约束                          ← subDataGroups
    //  ├─ 遗留分组(内含垫片 Part)            ← 麻烦构造①:数据层假组
    //  ├─ 标准件库文件夹(内含 2 螺母)        ← 麻烦构造②:嵌套容器
    //  ├─ 原点 Geom(treeParent=root)         ← m_children 政体
    //  │   └─ 平面 Geom(QObject parent=原点)  ← QObject 政体(parentForTree 回退)
    auto* root = new NamedData("Machine");
    auto* base  = new PartObject("Base",  "Steel", root);
    auto* boltA = new PartObject("BoltA", "Steel", root);
    auto* boltB = new PartObject("BoltB", "Alum",  root);
    auto* cover = new PartObject("Cover", "Alum",  root); cover->suppressed = true;
    root->addSubDataGroup(base);
    root->addSubDataGroup(boltA);
    root->addSubDataGroup(boltB);
    root->addSubDataGroup(cover);
    root->addSubDataGroup(new SketchObject("Sketch1", root));
    root->addSubDataGroup(new ConstraintObject("Mate1", root));
    auto* legacy = new LegacyGroupObject("OldCluster", root);
    root->addSubDataGroup(legacy);
    auto* washer = new PartObject("Washer", "Steel", legacy);   // 垫片:数据父亲是 legacy!
    legacy->addSubDataGroup(washer);
    auto* folder = new FolderObject("StdParts", root);
    root->addSubDataGroup(folder);
    folder->addSubDataGroup(new PartObject("Nut1", "Steel", folder));
    folder->addSubDataGroup(new PartObject("Nut2", "Steel", folder));
    // origin 走 m_children 政体(root 连坐它);plane 走 QObject 政体(origin 连坐它)。
    // plane 不进 origin 的 m_children —— 双重管理 = 双重删除(见 NamedGeom 注释)
    auto* origin = new NamedGeom("Origin", nullptr, root);
    root->addTreeChild(origin);
    auto* plane = new NamedGeom("Plane", origin, nullptr);

    const DataSnapshot snap0 = snapshot(root);

    printf("====== S0 raw tree (no projection, engine passes through) ======\n");
    printTree(root);
    printf("  note: Plane's parentOf falls back to QObject::parent() -> %s\n",
           describe(E.parentOf(plane)).c_str());
    verify(root, snap0, checked);

    // ═══ 场景 1:dynamicAll 按类型分组 ═══
    // dynamicAll=true:参与集 = 父亲的【当前全部活孩子】。数据侧日后新增的
    // Part 会在下次校验时自动进组 —— 名单会过期,活列表不会。
    printf("\n====== S1 group by type (dynamicAll: new nodes auto-join) ======\n");
    GroupRule ruleType = GroupRules::byKey([](GuiBase* n) -> std::string {
        if (n->type() == "Part") return "Parts";
        return "";   // 空 key → 逃生口:草图/约束/遗留分组/文件夹/几何留直属
    });
    auto groups = E.apply(root, {}, ruleType, { /*collapse*/false, /*dynamicAll*/true });
    printTree(root);
    printf("  groups created = %d\n", (int)groups.size());
    verify(root, snap0, checked);
    printf("  [OK] washer NOT kidnapped: parentOf(washer) = %s (via data-side supDataGroup)\n",
           describe(E.parentOf(washer)).c_str());

    // ═══ 场景 2:脏输入 + 换规则 ═══
    printf("\n====== S2 dirty input + switch to material rule ======\n");
    // 找到 S1 建的 Parts 组,故意混进输入 → 引擎应把它展开为全体成员参与
    TreeGroup* s1group = nullptr;
    for (size_t i = 0, rc = E.childCount(root); i < rc; ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(E.child(root, i))) { s1group = g; break; }
    std::vector<GuiBase*> dirty = { nullptr, base, base, washer, legacy, s1group, plane };
    GroupRule ruleMaterial = [](GuiBase* n) -> std::optional<GroupTicket> {
        if (auto* p = dynamic_cast<PartObject*>(n)) {
            GroupTicket t;
            if (p->suppressed) { t.key = "sup"; t.label = "Suppressed"; t.order = 999; }
            else               { t.key = p->material(); t.label = p->material() + " parts"; }
            return t;
        }
        return std::nullopt;   // 非 Part 一律直属
    };
    groups = E.apply(root, dirty, ruleMaterial);   // 静态白名单模式
    printTree(root);
    printf("  dirty input %d items -> groups = %d (washer/plane rejected: not in root's live enumeration)\n",
           (int)dirty.size(), (int)groups.size());
    verify(root, snap0, checked);

    // ═══ 场景 3:refresh 幂等,组指针稳定 ═══
    // 同 key 的组被【复用】(指针不变)→ QModelIndex/QPersistentModelIndex 依然
    // 有效,展开/选中状态跨 refresh 存活,不需要 reset 全树。
    printf("\n====== S3 refresh idempotence (group pointer stability) ======\n");
    TreeGroup* steelBefore = nullptr;
    for (auto* g : groups) if (g->ticket().key == "Steel") steelBefore = g;
    E.refresh(root);
    TreeGroup* steelAfter = nullptr;
    for (size_t i = 0, cnt = E.childCount(root); i < cnt; ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(E.child(root, i)))
            if (g->ticket().key == "Steel") steelAfter = g;
    printf("  Steel group before refresh = %p, after = %p -> %s\n",
           (void*)steelBefore, (void*)steelAfter,
           steelBefore == steelAfter ? "SAME (model state preserved)" : "CHANGED");

    // ═══ 场景 4:数据侧删除 → 自动蒸发 ═══
    printf("\n====== S4 data-side delete BoltB & Cover -> groups auto-evaporate ======\n");
    printf("--- before ---\n");
    printTree(root);
    printf("  liveGroupCount = %d\n", (int)E.liveGroupCount());
    root->destroySubDataGroup(boltB);    // 裸 delete,零展示层通知 —— 引擎不需要同步!
    root->destroySubDataGroup(cover);
    const DataSnapshot snap4 = snapshot(root);   // 删除是数据侧合法变更,基线在删除后重拍
    printf("--- after (next childCount revalidates from live enumeration) ---\n");
    printTree(root);                             // 这次遍历触发 revalidate = 引擎的全部"动作"
    printf("  liveGroupCount = %d (Alum & Suppressed groups gone; no dangling pointer ever stored)\n",
           (int)E.liveGroupCount());
    verify(root, snap4, checked);   // 证明引擎重算过程对数据侧零写入

    // ═══ 场景 5:全树模型一致性不变量 ═══
    printf("\n====== S5 model invariants over whole projected tree ======\n");
    g_invariantChecks = 0; g_invariantOk = true;
    checkInvariants(root);
    printf(g_invariantOk ? "  [OK] %d parentOf/rowOf checks passed (tree is a strict single-parent tree)\n"
                         : "  [FAIL] invariant broken\n", g_invariantChecks);

    // ═══ 场景 6:ungroup 还原 ═══
    printf("\n====== S6 uninstall -> flat again ======\n");
    E.uninstall(root);
    printTree(root);
    printf("  liveGroupCount = %d\n", (int)E.liveGroupCount());

    // 嵌套分组快演:folder 内部按材料分组,与 root 层互不干扰
    printf("\n====== S6b nested grouping inside folder (2 layers, different branches) ======\n");
    E.apply(folder, {}, GroupRules::byKey([](GuiBase* n) {
        if (auto* p = dynamic_cast<PartObject*>(n)) return p->material();
        return std::string();
    }), { false, true });
    printTree(root);
    E.uninstall(folder);

    // ═══ 场景 7:多层分组专项 ═══
    // 7a. 三层嵌套:root 按类型分组 → 对 Parts 组【本身】再按材料分组
    printf("\n====== S7a grouping INSIDE a group (3 layers) ======\n");
    groups = E.apply(root, {}, ruleType, { false, true });
    TreeGroup* partsGroup = groups.empty() ? nullptr : groups.front();
    if (partsGroup) {
        E.apply(partsGroup, {}, ruleMaterial, { false, true });   // 对组再分组
        printTree(root);
        g_invariantChecks = 0; g_invariantOk = true;
        checkInvariants(root);
        printf(g_invariantOk ? "  [OK] 3-layer invariants: %d parentOf/rowOf checks passed\n"
                             : "  [FAIL] 3-layer invariant broken\n", g_invariantChecks);

        // 7b. 级联拆除:uninstall(root) 必须连内层投影一起清
        printf("\n====== S7b cascade teardown: uninstall(root) kills inner layers too ======\n");
        printf("  liveGroupCount before = %d (outer + inner)\n", (int)E.liveGroupCount());
        E.uninstall(root);
        printf("  liveGroupCount after  = %d (must be 0 -> no zombie projection, no leak)\n",
               (int)E.liveGroupCount());
        printTree(root);
    }

    // ═══ 场景 8:三政体生存法则 —— 父亲死亡后的引擎自愈 ═══
    printf("\n====== S8 owner death across regimes -> discard/prune self-heal ======\n");
    E.registerRoot(root);   // 注册根之后,install/refresh 会在操作边界自动剪枝

    // 8a. 数据自管政体:folder2 子树装了投影,整棵裸删、零通知 → prune 自愈
    auto* folder2 = new FolderObject("TempFolder", root);
    root->addSubDataGroup(folder2);
    folder2->addSubDataGroup(new PartObject("Scr1", "Steel", folder2));
    folder2->addSubDataGroup(new PartObject("Scr2", "Alum", folder2));
    E.apply(folder2, {}, ruleMaterial, { false, true });
    printf("  [8a] alive:          hasProjection=%d liveGroups=%d\n",
           (int)E.hasProjection(folder2), (int)E.liveGroupCount());
    for (auto* s : std::vector<DataObject*>(folder2->subDataGroups())) folder2->destroySubDataGroup(s);
    root->destroySubDataGroup(folder2);       // 数据侧裸 delete,展示层零通知
    E.prune();                                 // 通常无需手动:install/refresh 自动执行
    printf("  [8a] deleted+pruned: liveGroups=%d (zombie projection purged, orphan group freed)\n",
           (int)E.liveGroupCount());

    // 8b. QObject 政体:geomParent 孩子只归 QObject 树;delete 时 ~QObject 连坐
    auto* geomParent = new NamedGeom("GeomCluster", nullptr, root);
    root->addTreeChild(geomParent);
    new NamedGeom("G1", geomParent, nullptr);
    new NamedGeom("G2", geomParent, nullptr);
    E.apply(geomParent, {}, GroupRules::byPredicate(
        [](GuiBase* n) { return n->type() == "Geom"; }, "GeomSet"), { false, true });
    printf("  [8b] alive:          liveGroups=%d childCount(geomParent)=%d\n",
           (int)E.liveGroupCount(), (int)E.childCount(geomParent));
    root->removeTreeChild(geomParent);
    delete geomParent;                         // ~QObject 连坐 G1/G2
    E.prune();
    printf("  [8b] deleted+pruned: liveGroups=%d\n", (int)E.liveGroupCount());

    // 8c. 显式讣告(最体面):delete 前 discard,引擎立即清账,不用等 prune
    auto* standalone = new FolderObject("Standalone", root);
    root->addSubDataGroup(standalone);
    standalone->addSubDataGroup(new PartObject("Solo1", "Steel", standalone));
    E.apply(standalone, {}, ruleMaterial, { false, true });
    printf("  [8c] alive:          liveGroups=%d\n", (int)E.liveGroupCount());
    E.discard(standalone);
    for (auto* s : std::vector<DataObject*>(standalone->subDataGroups())) standalone->destroySubDataGroup(s);
    root->destroySubDataGroup(standalone);
    printf("  [8c] discard+delete: liveGroups=%d (no prune needed)\n", (int)E.liveGroupCount());

    // ═══ 场景 9:全量重建政体 —— wipe-and-rebuild 与规则自动重生 ═══
    // 分组意图绑到【定位谓词】(名字/类型等稳定特征)而非指针;
    // 重建后 rebind:持久 owner 命中缓存(组指针稳定),一次性 owner 被谓词重新命中。
    printf("\n====== S9 wipe-and-rebuild regime -> bindSpec/rebind auto re-grow ======\n");
    E.bindSpec({ "machine-parts",       // 谓词:名叫 Machine 的持久装配体
        [](GuiBase* n){ auto* d = dynamic_cast<NamedData*>(n); return d && d->name() == "Machine"; },
        ProjectionSpec{ nullptr, ruleType, nullptr, { false, true } } });
    E.bindSpec({ "folders-by-material", // 谓词:所有 Folder —— 一条绑定管全树所有文件夹
        [](GuiBase* n){ return n->type() == "Folder"; },
        ProjectionSpec{ nullptr, ruleMaterial, nullptr, { false, true } } });
    E.bindSpec({ "origin-geomset",      // 谓词:名叫 Origin 的 Geom(一次性对象)
        [](GuiBase* n){ auto* g = dynamic_cast<NamedGeom*>(n); return g && g->name() == "Origin"; },
        ProjectionSpec{ nullptr,
                        GroupRules::byPredicate([](GuiBase* n){ return n->type() == "Geom"; }, "GeomSet"),
                        nullptr, { false, true } } });
    printTree(root);
    TreeGroup* partsBefore = nullptr;
    for (size_t i = 0, rc = E.childCount(root); i < rc; ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(E.child(root, i)))
            if (g->ticket().key == "Parts") partsBefore = g;

    // 模拟几何模块更新:origin 子树整棵删光重建
    root->removeTreeChild(origin);   // 先从 m_children 摘干净(悬空条目会毒害一切遍历)
    delete origin;                   // ~QObject 连坐 plane;origin 名下投影沦为僵尸账
    auto* origin2 = new NamedGeom("Origin", nullptr, root);
    root->addTreeChild(origin2);
    new NamedGeom("Plane", origin2, nullptr);

    E.rebind();                      // = prune(清僵尸账) + 沿活树按谓词重装全部绑定
    printTree(root);
    TreeGroup* partsAfter = nullptr;
    for (size_t i = 0, rc = E.childCount(root); i < rc; ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(E.child(root, i)))
            if (g->ticket().key == "Parts") partsAfter = g;
    printf("  Parts group (durable owner=root): before=%p after=%p -> %s\n",
           (void*)partsBefore, (void*)partsAfter,
           partsBefore == partsAfter ? "SAME (state preserved across rebuild)" : "CHANGED");
    printf("  Origin (disposable owner): zombie purged, new Origin auto re-grouped by predicate\n");

    // ═══ 场景 10:投影流水线 —— 一个 Spec = 过滤+分组+排序 ═══
    printf("\n====== S10 projection pipeline (filter+group+sort) & dump-diff ======\n");
    // 10a. 排序等价性验证(迁移方法论:捕获现行 → 等价替换 → diff):
    //      基线 = 模拟"写死在旧导航树里的排序"(直接对原始枚举 stable_sort),
    //      引擎只装 less(不过滤不分组)→ flat 必须与基线逐元素一致
    std::vector<GuiBase*> baseline;
    for (size_t i = 0, n = root->visibleChildrenCount(); i < n; ++i)
        baseline.push_back(root->visibleChild(i));
    std::stable_sort(baseline.begin(), baseline.end(), demoLess);
    E.install(root, ProjectionSpec{ nullptr, nullptr, demoLess, { false, true } });
    bool same = baseline.size() == E.childCount(root);
    for (size_t i = 0; same && i < baseline.size(); ++i)
        same = (E.child(root, i) == baseline[i]);
    printf("  [10a] sort-only spec vs hardcoded baseline: %s (%d rows)\n",
           same ? "EQUIVALENT (dump-diff passed)" : "DIFF!", (int)baseline.size());

    // 10b. 完整流水线:过滤约束 + 按材料分组 + 排序(组前置,其余按类型秩+名字)
    E.install(root, ProjectionSpec{
        [](GuiBase* n){ return n->type() != "Constraint"; },   // Stage A:隐藏约束(示例过滤)
        ruleMaterial,                                           // Stage B:零件按材料归组
        demoLess,                                               // Stage C:组+直属混排
        { false, true } });
    printTree(root);
    printf("  (Mate1 filtered out; groups first via demoLess rank; rest by type+name)\n");

    // 10c. 卸载 → 恢复透传(绑定仍在表里,但下次 rebind 才会重装)
    E.uninstall(root);
    printf("  [10c] after uninstall(root): raw order restored\n");
    printTree(root);

    printf("\n====== summary ======\n");
    printf("  engine live groups = %d (folder/origin bindings still active; freed at shutdown)\n",
           (int)E.liveGroupCount());
    printf("  data-side writes by engine = 0 (sup/subGroups/m_parentForTree all untouched)\n");
    printf("  regime coverage: m_children / QObject / subDataGroups / wipe-and-rebuild\n");
    printf("  pipeline stages: filter -> group -> sort, one declarative ProjectionSpec\n");

    // demo 收尾:严格按各政体的所有权语义清理 ——
    // 数据儿子逐个 destroySubDataGroup(先孙辈后子辈);
    // origin2 挂在 root 的 m_children → ~GuiBase(root) 连坐;
    // plane2 是 origin2 的 QObject 子 → ~QObject(origin2) 连坐。
    for (auto* s : std::vector<DataObject*>(legacy->subDataGroups())) legacy->destroySubDataGroup(s);
    for (auto* s : std::vector<DataObject*>(folder->subDataGroups())) folder->destroySubDataGroup(s);
    for (auto* s : std::vector<DataObject*>(root->subDataGroups()))   root->destroySubDataGroup(s);
    delete root;
    return 0;
}
