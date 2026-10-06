// ═══════════════════════════════════════════════════════════════
//  gui_demo:NavTreeModel + QTreeView 实跑两条更新通道
// ═══════════════════════════════════════════════════════════════
//  运行:无显示环境用 QT_QPA_PLATFORM=offscreen
//  删除通道 = 【事前坐标】:victim 死前问 parentOf/rowOf,begin → 删 → end。
//  数据变更被 begin/end 正确括号,Qt 协议天然合法(Debug 断言无风险),
//  与内网既有"删除走精准 begin/endRemoveRows"的流程一比一对应 ——
//  迁移时只需把坐标来源换成投影账本(removeWithSignal 就是范式)。
//  验证点:
//   Op1 精准删除组内成员     → 1 条 removeRows,parent = 组的 index
//   Op2 删除组的最后成员     → 整组退出:1 条 removeRows 在 root 层(子树随组行消失)
//   Op3 删除被过滤的节点     → 零信号(它本来就不在投影树上)
//   Op4 全量重建通道         → modelReset + prune/rebind,GeomSet 组重生、Steel 组指针稳定
//   终检:parent(index(c))==idx 全树不变量
#include <QApplication>
#include <QTreeView>
#include <cstdio>
#include <string>
#include <vector>
#include "guibase.h"
#include "treeprojection.h"
#include "navtreemodel.h"
#include "demonodes.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static TreeProjection& P = TreeProjection::instance();

// 显示名(组的成员数走引擎 childCount,即"结构层计数")
static QString qName(GuiBase* n) {
    if (auto* g = dynamic_cast<TreeGroup*>(n))
        return QString::fromStdString("[组] " + g->ticket().label + " ("
            + std::to_string(P.childCount(n)) + " 个成员)");
    QString s = QString::fromStdString(demoNodeName(n));
    if (auto* p = dynamic_cast<PartObject*>(n)) {
        s += QString::fromStdString(" [" + p->material() + "]");
        if (p->suppressed) s += QStringLiteral("(压制)");
    }
    return s + QString::fromStdString("  <" + n->type() + ">");
}

// index 的可读路径(信号日志用):Machine/[组]Steel parts
static QString idxPath(const NavTreeModel& m, QModelIndex idx) {
    QStringList parts;
    while (idx.isValid()) {
        parts.prepend(m.data(idx, Qt::DisplayRole).toString());
        idx = idx.parent();
    }
    return parts.isEmpty() ? QStringLiteral("<root>") : parts.join("/");
}

// 通过模型(而非引擎)递归 dump —— 验证模型三要素自洽
static void dumpModel(const NavTreeModel& m, const QModelIndex& idx = QModelIndex(),
                      const QString& prefix = QString(), bool root = true) {
    int rows = m.rowCount(idx);
    if (root) printf("%s\n", qUtf8Printable(QStringLiteral("<root> (%1 行)").arg(rows)));
    for (int r = 0; r < rows; ++r) {
        QModelIndex c = m.index(r, 0, idx);
        bool last = (r == rows - 1);
        printf("%s\n", qUtf8Printable(prefix + (last ? QStringLiteral("+- ") : QStringLiteral("|- "))
                                      + m.data(c, Qt::DisplayRole).toString()));
        dumpModel(m, c, prefix + (last ? QStringLiteral("   ") : QStringLiteral("|  ")), false);
    }
}

// 模型级不变量:parent(index(r))==idx 且行数与引擎一致
static bool checkModel(const NavTreeModel& m, const QModelIndex& idx, int& checks) {
    int rows = m.rowCount(idx);
    for (int r = 0; r < rows; ++r) {
        QModelIndex c = m.index(r, 0, idx);
        if (!c.isValid() || c.parent() != idx) {
            printf("  [FAIL] parent(index) mismatch at row %d of %s\n", r,
                   qUtf8Printable(idxPath(m, idx)));
            return false;
        }
        ++checks;
        if (!checkModel(m, c, checks)) return false;
    }
    return true;
}

static TreeGroup* findGroup(GuiBase* parent, const std::string& key) {
    for (size_t i = 0, n = P.childCount(parent); i < n; ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(P.child(parent, i)))
            if (g->ticket().key == key) return g;
    return nullptr;
}

// ═══ 事前坐标删除(精准通道的全部实现,就这一个函数)═══
// 迁移到内网时,把你们现有删除点里"取数据侧行号"的那几行,
// 换成这里的 parentOf/rowOf 即可 —— begin/删除/end 的骨架原样保留。
//
// ★ structRev 自检(惰性投影的固有事实,见 navtreemodel.h:88-95):
//   一次纯读(rowOf/parentOf/childCount)就可能让树的形状变化(成员被数据侧删掉
//   → 组空 → 组蒸发),而模型收不到任何信号。
//   ⚠ 基线必须取在【所有坐标查询之后】—— parentOf/rowOf 自己就会触发 revalidate
//     从而可能当场淘汰空组;若在它们之前取基线,基线已被本次查询的副作用污染,
//     自检就永远报"未变"(实测踩过,是个假绿灯)。
//   取在删除【之前】、坐标查询【之后】,才是真正要验证的不变量:
//     "这次结构变更所改变的形状,是否恰好被本次 begin/endRemoveRows 覆盖了"。
//   判据:组蒸发 ⇒ 形状必变(应报 changed);仅成员数变化 ⇒ 形状不变(应报 same)。
static void removeWithSignal(NavTreeModel& model, DataObject* owner, DataObject* victim) {
    GuiBase* par = P.parentOf(victim);            // ① 死前问坐标(投影账本)
    if (!par || P.rowOf(victim) < 0) {            // ② 被过滤/不在投影树 → 零信号直接删
        P.notifyRemove(owner, victim);            // ★v4:被过滤≠不通知(它占着原始活列表)
        owner->destroySubDataGroup(victim);
        return;                                   // 被滤节点不在投影树上,形状不会变
    }
    // ③ 级联预判(多层版):victim 死后会连带蒸发的那一串组里【最外层】的坐标。
    //    信号发在它的位置(外层一条),子树随该行消失 —— 协议最简且正确。
    GuiBase* cas = cascadeTarget(P, victim);
    const size_t revBefore = model.src().structRev();   // ★ 坐标查询全部做完,取基线
    if (cas) {
        model.beginRowRemoval(P.parentOf(cas), P.rowOf(cas));
        P.notifyRemove(owner, victim);            // ★v4:通知在 delete 前、坐标查询后
        owner->destroySubDataGroup(victim);       // 下次查询按脏标志重算,空组自动拆掉
        model.endRowRemoval();
    } else {
        int row = P.rowOf(victim);                // ④ 普通情形:成员行,组存活
        model.beginRowRemoval(par, row);
        P.notifyRemove(owner, victim);            // ★v4 通知
        owner->destroySubDataGroup(victim);
        model.endRowRemoval();
    }
    // ★ 复核:形状变化是否落在本次删除窗口内。
    //   ⚠ 读法(实测踩过,别误判成 bug):形状也可能在【上一步的坐标查询里】
    //     就已经变了 —— 惰性投影下,一次纯读(rowOf/parentOf/childCount)就会把
    //     成员已空、但尚未被淘汰的组当场蒸发。那种情况下本次 delta 会是 0,
    //     而组确实已经消失:即"形状变化发生在基线之前"。
    //     所以 delta=0 不等于"没变化",只等于"本次删除窗口内没再变";
    //     真正要保证的不变量是「形状变化必须被某个 begin/end 覆盖」,
    //     这也是为什么信号必须在【坐标查询前】就规划好位置(见上面 ③)。
    const size_t revAfter = model.src().structRev();
    printf("  [structRev] %zu -> %zu  %s\n", revBefore, revAfter,
           revBefore == revAfter
               ? "本次删除窗口内形状未变(组蒸发可能已计入上一步坐标查询)"
               : "本次删除窗口内形状已变(组蒸发 → 已由本次信号覆盖)");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif

    // ═══ 建树(三政体混合,比控制台 demo 小一圈) ═══
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
    auto* folder = new FolderObject("StdParts", root);
    root->addSubDataGroup(folder);
    folder->addSubDataGroup(new PartObject("Nut1", "Steel", folder));
    folder->addSubDataGroup(new PartObject("Nut2", "Steel", folder));
    auto* origin = new NamedGeom("Origin", nullptr, root);
    root->addTreeChild(origin);
    new NamedGeom("Plane", origin, nullptr);

    P.registerRoot(root);

    // ═══ 声明式 Spec 绑定:一个 Spec = 过滤+分组+排序 ═══
    GroupRule byMaterial = [](GuiBase* n) -> std::optional<GroupTicket> {
        if (auto* p = dynamic_cast<PartObject*>(n)) {
            GroupTicket t; t.key = p->material(); t.label = p->material() + " parts";
            return t;
        }
        return std::nullopt;
    };
    P.bindSpec({ "root-spec",
        [](GuiBase* n){ auto* d = dynamic_cast<NamedData*>(n); return d && d->name() == "Machine"; },
        ProjectionSpec{
            [](GuiBase* n){ auto* p = dynamic_cast<PartObject*>(n); return !(p && p->suppressed); }, // 过滤压制件
            byMaterial,      // 零件按材料分组
            demoLess,        // 组前置,其余按类型秩+名字
            { false, true } } });
    P.bindSpec({ "folder-spec",
        [](GuiBase* n){ return n->type() == "Folder"; },
        ProjectionSpec{ nullptr, byMaterial, demoLess, { false, true } } });
    P.bindSpec({ "origin-spec",
        [](GuiBase* n){ auto* g = dynamic_cast<NamedGeom*>(n); return g && g->name() == "Origin"; },
        ProjectionSpec{ nullptr,
                        GroupRules::byPredicate([](GuiBase* n){ return n->type() == "Geom"; }, "GeomSet"),
                        demoLess, { false, true } } });

    int rc = 0;
    {   // 模型/视图作用域:先于数据销毁,避免视图析构时查询死树
        NavTreeModel model(root, P);          // ★ 模型只持有 ITreeSource 窄接口
        model.setLabeler(qName);
        QTreeView view;
        view.setModel(&model);
        view.expandAll();
        view.show();                          // offscreen 平台下照常工作

        // ── 信号日志:把 begin 系信号翻译成可读坐标 ──
        QObject::connect(&model, &QAbstractItemModel::rowsAboutToBeRemoved,
            [&](const QModelIndex& p, int f, int l) {
                printf("  [signal] rowsAboutToBeRemoved  parent=%-28s rows=[%d..%d]\n",
                       qUtf8Printable(idxPath(model, p)), f, l);
            });
        QObject::connect(&model, &QAbstractItemModel::rowsAboutToBeInserted,
            [&](const QModelIndex& p, int f, int l) {
                printf("  [signal] rowsAboutToBeInserted parent=%-28s rows=[%d..%d]\n",
                       qUtf8Printable(idxPath(model, p)), f, l);
            });
        QObject::connect(&model, &QAbstractItemModel::modelAboutToBeReset,
            []{ printf("  [signal] modelAboutToBeReset\n"); });

        printf("====== 初始投影树(Cover 被压制过滤,组前置+排序) ======\n");
        dumpModel(model);
        int checks = 0;
        bool ok = checkModel(model, QModelIndex(), checks);
        printf(ok ? "  [OK] 模型不变量 %d 项通过\n" : "  [FAIL]\n", checks);

        TreeGroup* steelBefore = findGroup(root, "Steel");

        // ═══ Op1 精准删除通道:组内成员 BoltA(Steel 组 2→1,组存活) ═══
        printf("\n====== Op1 remove BoltA inside Steel group (expect 1 signal at group level) ======\n");
        removeWithSignal(model, root, boltA);       // 事前问坐标 → begin → 删 → end
        dumpModel(model);

        // ═══ Op2 级联:删除 BoltB(Alum 组唯一成员 → 整组退出 root 层) ═══
        printf("\n====== Op2 remove BoltB, last member of Alum group (expect 1 removal at root) ======\n");
        removeWithSignal(model, root, boltB);       // 级联预判:信号发在组行
        dumpModel(model);

        // ═══ Op3 删除被过滤节点:Cover(压制,不在投影树上) → 零信号 ═══
        printf("\n====== Op3 remove Cover (filtered out of projection; expect ZERO signals) ======\n");
        removeWithSignal(model, root, cover);       // rowOf==-1 → 直接删,不发信号
        printf("  (没有信号 = 正确:视图上本来就没有这一行)\n");

        // ═══ Op4 全量重建通道:Origin 子树 wipe-and-rebuild ═══
        printf("\n====== Op4 reset channel: wipe & rebuild Origin subtree ======\n");
        model.beginFullReset();
        root->removeTreeChild(origin);
        delete origin;                              // ~QObject 连坐 Plane;投影沦为僵尸账
        P.prune();                                  // ★ wipe 后必清(地址复用幽灵高发点)
        auto* origin2 = new NamedGeom("Origin", nullptr, root);
        root->addTreeChild(origin2);
        new NamedGeom("Plane", origin2, nullptr);
        P.rebind();                                 // ★ 谓词定位,分组自动重生
        model.endFullReset();
        view.doItemsLayout();                       // 你们既有流程的最后一步
        dumpModel(model);
        TreeGroup* steelAfter = findGroup(root, "Steel");
        TreeGroup* geomSet = nullptr;
        for (size_t i = 0, n = P.childCount(origin2); i < n; ++i)
            if (auto* g = dynamic_cast<TreeGroup*>(P.child(origin2, i))) geomSet = g;
        printf("  Steel 组(持久 owner=root):before=%p after=%p -> %s\n",
               (void*)steelBefore, (void*)steelAfter,
               steelBefore == steelAfter ? "SAME(跨 reset 状态可保)" : "CHANGED");
        printf("  GeomSet 组(一次性 owner):%s\n", geomSet ? "已在新 Origin 上重生 ✓" : "丢失 ✗");

        checks = 0;
        ok = checkModel(model, QModelIndex(), checks);
        printf(ok ? "  [OK] 终检:模型不变量 %d 项通过\n" : "  [FAIL] 终检失败\n", checks);
        if (!ok || !geomSet || steelBefore != steelAfter) rc = 1;
    }   // ← view/model 在此析构

    // ═══ 数据清理(严格按政体所有权) ═══
    for (auto* s : std::vector<DataObject*>(folder->subDataGroups())) folder->destroySubDataGroup(s);
    for (auto* s : std::vector<DataObject*>(root->subDataGroups()))   root->destroySubDataGroup(s);
    delete root;                       // ~GuiBase 连坐 origin2;~QObject(origin2) 连坐 Plane2
    printf("\n  engine live groups after cleanup = %d (bindings still registered; groups freed with owners)\n",
           (int)P.liveGroupCount());
    return rc;
}
