// ═══════════════════════════════════════════════════════════════
//  tree_tabs:把全部 case 画进真窗口,一个 tab 一个场景
// ═══════════════════════════════════════════════════════════════
//  Tab① 透传原始树(三政体混合,零投影)
//  Tab② 按类型分组(dynamicAll)
//  Tab③ 过滤压制件 + 按材料分组
//  Tab④ 多层分组(root 按类型 → Part 组内再按材料,三层树)
//  Tab⑤ 精准删除(事前坐标四行骨架,按钮触发 + 信号日志)
//  Tab⑥ 全量重建(reset 通道:wipe → prune → rebuild → rebind)
//  Tab⑦ 排序流水线对比(原始序 / 只排序 / 完整流水线,三树并排)
//
//  运行:正常双击即可;无显示环境 QT_QPA_PLATFORM=offscreen
//  自检:加参数 --selftest → 自动执行 Tab⑤⑥ 的操作并校验,退出码=结果
#include <QApplication>
#include <QTabWidget>
#include <QTreeView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTimer>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include "guibase.h"
#include "treeprojection.h"
#include "navtreemodel.h"
#include "demonodes.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static TreeProjection& P = TreeProjection::instance();

// 清理登记(tab 构建时 push,main 里窗口析构后执行 —— 视图/模型必须先于数据死)
static std::vector<std::function<void()>> g_extraCleanups;

// ═══════════════ 公共小件 ═══════════════

// 显示名(组成员数走引擎 childCount,即"结构层计数")
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

// 一棵标准演示树(三政体混合):数据侧零件 + 文件夹 + QObject 政体几何
struct Machine {
    NamedData* root = nullptr;
    PartObject *base = nullptr, *boltA = nullptr, *boltB = nullptr, *cover = nullptr;
    SketchObject* sketch = nullptr;
    FolderObject* folder = nullptr;
    NamedGeom *origin = nullptr, *plane = nullptr;
};

static Machine buildMachine(const std::string& rootName, bool suppressCover) {
    Machine M;
    M.root  = new NamedData(rootName);
    M.base  = new PartObject("Base",  "Steel", M.root);
    M.boltA = new PartObject("BoltA", "Steel", M.root);
    M.boltB = new PartObject("BoltB", "Alum",  M.root);
    M.cover = new PartObject("Cover", "Alum",  M.root);
    M.cover->suppressed = suppressCover;
    M.root->addSubDataGroup(M.base);
    M.root->addSubDataGroup(M.boltA);
    M.root->addSubDataGroup(M.boltB);
    M.root->addSubDataGroup(M.cover);
    M.sketch = new SketchObject("Sketch1", M.root);
    M.root->addSubDataGroup(M.sketch);
    M.folder = new FolderObject("StdParts", M.root);
    M.root->addSubDataGroup(M.folder);
    M.folder->addSubDataGroup(new PartObject("Nut1", "Steel", M.folder));
    M.folder->addSubDataGroup(new PartObject("Nut2", "Alum",  M.folder));
    // QObject 政体:origin 挂 root 的 m_children(展示),Plane 挂 origin 的 QObject 树
    M.origin = new NamedGeom("Origin", nullptr, M.root);
    M.root->addTreeChild(M.origin);
    M.plane  = new NamedGeom("Plane", M.origin, nullptr);
    P.registerRoot(M.root);
    return M;
}

// 递归清空数据侧子树(严格按政体所有权)
static void destroyDataChildren(DataObject* d) {
    for (auto* s : std::vector<DataObject*>(d->subDataGroups())) {
        destroyDataChildren(s);
        d->destroySubDataGroup(s);        // 摘除 + delete
    }
}
static void destroyMachine(Machine& M) {
    P.unregisterRoot(M.root);
    destroyDataChildren(M.root);
    delete M.root;                        // ~GuiBase 连坐 origin;~QObject(origin) 连坐 Plane
    M = {};
}

// 视图工厂:模型 parent=视图(生命周期跟随),展开、隐藏表头
static QTreeView* makeView(GuiBase* root, QWidget* parent) {
    auto* v = new QTreeView(parent);
    auto* m = new NavTreeModel(root, P, v);       // ★ 模型只持有 ITreeSource 窄接口
    m->setLabeler(qName);
    v->setModel(m);
    v->setHeaderHidden(true);
    v->setUniformRowHeights(true);
    v->expandAll();
    return v;
}
static NavTreeModel* modelOf(QTreeView* v) { return static_cast<NavTreeModel*>(v->model()); }

// 信号日志:begin 系信号翻译成可读坐标追加到 log(以 log 为 context,同生共死)
static void hookSignals(NavTreeModel* m, QPlainTextEdit* log) {
    auto path = [m](QModelIndex idx) {
        QStringList ps;
        while (idx.isValid()) { ps.prepend(m->data(idx, Qt::DisplayRole).toString()); idx = idx.parent(); }
        return ps.isEmpty() ? QStringLiteral("<root>") : ps.join("/");
    };
    QObject::connect(m, &QAbstractItemModel::rowsAboutToBeRemoved, log,
        [log, path](const QModelIndex& p, int f, int l) {
            log->appendPlainText(QString("[signal] rowsAboutToBeRemoved  parent=%1  rows=[%2..%3]")
                                     .arg(path(p)).arg(f).arg(l)); });
    QObject::connect(m, &QAbstractItemModel::rowsAboutToBeInserted, log,
        [log, path](const QModelIndex& p, int f, int l) {
            log->appendPlainText(QString("[signal] rowsAboutToBeInserted parent=%1  rows=[%2..%3]")
                                     .arg(path(p)).arg(f).arg(l)); });
    QObject::connect(m, &QAbstractItemModel::modelAboutToBeReset, log,
        [log] { log->appendPlainText("[signal] modelAboutToBeReset"); });
}

// 带说明标题的简单 tab(标题 + 一棵树)
static QWidget* simpleTab(const QString& caption, GuiBase* root) {
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    auto* cap = new QLabel(caption);
    cap->setWordWrap(true);
    lay->addWidget(cap);
    lay->addWidget(makeView(root, w), 1);
    return w;
}

// 规则小件(与 gui_demo 同款)
static GroupRule byMaterial() {
    return [](GuiBase* n) -> std::optional<GroupTicket> {
        if (auto* p = dynamic_cast<PartObject*>(n)) {
            GroupTicket t; t.key = p->material(); t.label = p->material() + " parts";
            return t;
        }
        return std::nullopt;
    };
}
static std::function<bool(GuiBase*)> notSuppressed() {
    return [](GuiBase* n) { auto* p = dynamic_cast<PartObject*>(n); return !(p && p->suppressed); };
}
static TreeGroup* findGroup(GuiBase* parent, const std::string& key) {
    for (size_t i = 0, n = P.childCount(parent); i < n; ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(P.child(parent, i)))
            if (g->ticket().key == key) return g;
    return nullptr;
}

// ═══════════════ Tab⑤ 精准删除(事前坐标) ═══════════════

struct Tab5Ctx { Machine* M; NavTreeModel* model; QPlainTextEdit* log; };
static Tab5Ctx* g_t5 = nullptr;

// 事前坐标删除(与 gui_demo removeWithSignal 同款四行骨架 + 级联预判)
static void t5Remove(PartObject*& slot, const QString& what) {
    if (!slot) return;
    DataObject* victim = slot;
    g_t5->log->appendPlainText("---- " + what + " ----");
    GuiBase* par = P.parentOf(victim);                  // ① 死前问坐标
    if (!par || P.rowOf(victim) < 0) {                  // ② 被过滤 → 零信号直接删
        P.notifyRemove(g_t5->M->root, victim);          // ★v4 通知(delete 前)
        g_t5->M->root->destroySubDataGroup(victim);
        g_t5->log->appendPlainText("(rowOf<0:不在投影树上 → 未发任何信号)");
        slot = nullptr;
        return;
    }
    // ③ 级联预判(多层版):信号发在最外层会蒸发的组上
    if (GuiBase* cas = cascadeTarget(P, victim)) {
        g_t5->model->beginRowRemoval(P.parentOf(cas), P.rowOf(cas));
        P.notifyRemove(g_t5->M->root, victim);          // ★v4 通知
        g_t5->M->root->destroySubDataGroup(victim);
        g_t5->model->endRowRemoval();
    } else {                                            // ④ 普通成员行
        g_t5->model->beginRowRemoval(par, P.rowOf(victim));
        P.notifyRemove(g_t5->M->root, victim);          // ★v4 通知
        g_t5->M->root->destroySubDataGroup(victim);
        g_t5->model->endRowRemoval();
    }
    slot = nullptr;
}

static QWidget* buildTab5() {
    auto* M = new Machine(buildMachine("Machine-T5", /*suppressCover=*/true));
    P.install(M->root, ProjectionSpec{ notSuppressed(), byMaterial(), demoLess, { false, true } });

    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    auto* cap = new QLabel(QStringLiteral(
        "Tab⑤ 精准删除通道(事前坐标):删除点死前问 parentOf/rowOf → beginRowRemoval → 删 → endRowRemoval。\n"
        "预期:删 BoltA = 1 条信号@Steel 组层;删 BoltB(Alum 组最后成员)= 1 条信号@root(整组蒸发);"
        "删 Cover(被压制过滤,不在投影树上)= 零信号。"));
    cap->setWordWrap(true);
    lay->addWidget(cap);

    auto* btns = new QHBoxLayout;
    auto* b1 = new QPushButton(QStringLiteral("删 BoltA(组内成员)"));
    auto* b2 = new QPushButton(QStringLiteral("删 BoltB(组最后成员)"));
    auto* b3 = new QPushButton(QStringLiteral("删 Cover(被过滤)"));
    btns->addWidget(b1); btns->addWidget(b2); btns->addWidget(b3); btns->addStretch(1);
    lay->addLayout(btns);

    auto* split = new QSplitter(Qt::Vertical);
    auto* view = makeView(M->root, split);
    auto* log = new QPlainTextEdit(split);
    log->setReadOnly(true);
    log->setMaximumBlockCount(2000);
    split->addWidget(view);
    split->addWidget(log);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);

    g_t5 = new Tab5Ctx{ M, modelOf(view), log };
    hookSignals(g_t5->model, log);
    QObject::connect(b1, &QPushButton::clicked, w, [b1] { t5Remove(g_t5->M->boltA, QStringLiteral("删 BoltA")); b1->setEnabled(false); });
    QObject::connect(b2, &QPushButton::clicked, w, [b2] { t5Remove(g_t5->M->boltB, QStringLiteral("删 BoltB(级联预判)")); b2->setEnabled(false); });
    QObject::connect(b3, &QPushButton::clicked, w, [b3] { t5Remove(g_t5->M->cover, QStringLiteral("删 Cover(被过滤)")); b3->setEnabled(false); });
    return w;
}

// ═══════════════ Tab⑥ 全量重建(reset 通道) ═══════════════

struct Tab6Ctx { Machine* M; NavTreeModel* model; QTreeView* view; QPlainTextEdit* log; };
static Tab6Ctx* g_t6 = nullptr;

static GuiBase* topOf(GuiBase* n) {                    // 沿展示父亲链爬到根
    GuiBase* p = n;
    while (p) { GuiBase* q = p->parentForTree(); if (!q) break; p = q; }
    return p;
}

static void t6Rebuild() {
    Tab6Ctx* c = g_t6;
    c->log->appendPlainText("---- wipe & rebuild Origin(reset 通道) ----");
    GuiBase* root = c->M->root;

    c->model->beginFullReset();                        // ① beginResetModel 门面
    root->removeTreeChild(c->M->origin);               // ② wipe:摘除 + delete 旧子树
    delete c->M->origin;                               //    ~QObject 连坐 Plane;投影沦为僵尸账
    P.prune();                                         // ③ ★ 必做:清僵尸(地址复用幽灵高发点)
    auto* o2 = new NamedGeom("Origin", nullptr, root); // ④ rebuild:新子树
    root->addTreeChild(o2);
    new NamedGeom("Plane", o2, nullptr);
    new NamedGeom("YZ-Plane", o2, nullptr);
    P.rebind();                                        // ⑤ 谓词定位 → GeomSet 组自动重生
    c->model->endFullReset();                          // ⑥ endResetModel
    c->view->expandAll();
    c->view->doItemsLayout();                          // ⑦ 既有流程的最后一步
    c->M->origin = o2;

    TreeGroup* g = findGroup(o2, "GeomSet");
    c->log->appendPlainText(g ? QStringLiteral("结果:GeomSet 组已在新 Origin 上重生 ✓ (%1 个成员)")
                                  .arg(P.childCount(g))
                              : QStringLiteral("结果:GeomSet 组丢失 ✗"));
}

static QWidget* buildTab6() {
    auto* M = new Machine(buildMachine("Machine-T6", /*suppressCover=*/false));
    // 腿③声明式绑定:锚定"定位谓词"(名字+所属根)而非指针 → 跨 wipe-and-rebuild 重生。
    // locate 里用 topOf 限定只命中本 tab 的 Origin(多根共存时互不串扰)。
    P.bindSpec({ "tabs-origin-geomspec",
        [M](GuiBase* n) {
            auto* g = dynamic_cast<NamedGeom*>(n);
            return g && g->name() == "Origin" && topOf(g) == M->root;
        },
        ProjectionSpec{ nullptr,
                        GroupRules::byPredicate([](GuiBase* n) { return n->type() == "Geom"; }, "GeomSet"),
                        demoLess, { false, true } } });

    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    auto* cap = new QLabel(QStringLiteral(
        "Tab⑥ 全量重建通道(GeomObject=纯视图对象,数据更新时整棵 wipe-and-rebuild):\n"
        "beginFullReset → wipe → prune(★必做) → rebuild → rebind(谓词命中,分组重生) → endFullReset → doItemsLayout。"));
    cap->setWordWrap(true);
    lay->addWidget(cap);

    auto* btns = new QHBoxLayout;
    auto* b = new QPushButton(QStringLiteral("wipe && rebuild Origin 子树"));
    btns->addWidget(b); btns->addStretch(1);
    lay->addLayout(btns);

    auto* split = new QSplitter(Qt::Vertical);
    auto* view = makeView(M->root, split);
    auto* log = new QPlainTextEdit(split);
    log->setReadOnly(true);
    split->addWidget(view);
    split->addWidget(log);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);

    g_t6 = new Tab6Ctx{ M, modelOf(view), view, log };
    hookSignals(g_t6->model, log);
    QObject::connect(b, &QPushButton::clicked, w, [] { t6Rebuild(); });
    return w;
}

// ═══════════════ Tab⑦ 排序流水线对比 ═══════════════

static QWidget* buildTab7() {
    auto* MA = new Machine(buildMachine("Machine-T7A", true));
    auto* MB = new Machine(buildMachine("Machine-T7B", true));
    auto* MC = new Machine(buildMachine("Machine-T7C", true));
    // A: 不装投影 → 引擎透传(= 数据侧原始顺序,即"写死排序迁移前"的基线形态)
    // B: 只装 less → 与写死基线做 dump-diff 的等价替换形态(S10a)
    P.install(MB->root, ProjectionSpec{ nullptr, nullptr, demoLess, {} });
    // C: 完整流水线 → 过滤压制件 + 材料分组 + 排序(S10b)
    P.install(MC->root, ProjectionSpec{ notSuppressed(), byMaterial(), demoLess, { false, true } });

    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    auto* cap = new QLabel(QStringLiteral(
        "Tab⑦ 同一棵数据树的三种投影(左→右):原始顺序透传 / 只装排序(迁移写死逻辑的等价替换形态) / "
        "完整流水线(过滤压制件+材料分组+排序)。三份数据完全同构,差异全部来自 ProjectionSpec。"));
    cap->setWordWrap(true);
    lay->addWidget(cap);
    auto* row = new QHBoxLayout;
    for (auto pair : { std::pair<const char*, Machine*>{ "A · 原始顺序(透传)", MA },
                       { "B · 只排序(demoLess)", MB },
                       { "C · 过滤+分组+排序", MC } }) {
        auto* col = new QVBoxLayout;
        col->addWidget(new QLabel(QString::fromUtf8(pair.first)));
        col->addWidget(makeView(pair.second->root, w), 1);
        row->addLayout(col, 1);
    }
    lay->addLayout(row, 1);
    // 三份数据都登记清理(挂在 w 销毁后统一执行,见 main 的 cleanups)
    g_extraCleanups.push_back([MA, MB, MC] { destroyMachine(*MA); destroyMachine(*MB); destroyMachine(*MC);
                                             delete MA; delete MB; delete MC; });
    return w;
}

// ═══════════════ 自检(--selftest:offscreen 下自动跑 Tab⑤⑥ 并校验) ═══════════════

static int runSelfTest() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        printf(ok ? "  [OK] %s\n" : "  [FAIL] %s\n", what);
        if (!ok) ++fails;
    };
    printf("== tree_tabs selftest ==\n");

    // Tab⑤:三次删除的信号通道正确性(用引擎账本核验结构)
    t5Remove(g_t5->M->boltA, "selftest: remove BoltA");
    TreeGroup* steel = findGroup(g_t5->M->root, "Steel");
    check(steel && P.childCount(steel) == 1, "T5 删 BoltA 后 Steel 组剩 1 成员");
    t5Remove(g_t5->M->boltB, "selftest: remove BoltB");
    check(!findGroup(g_t5->M->root, "Alum"), "T5 删 BoltB 后 Alum 组整组蒸发");
    // 被过滤节点:死【前】rowOf 就是 -1(绝不在死后查询死者指针)
    bool coverHidden = P.rowOf(g_t5->M->cover) == -1;
    t5Remove(g_t5->M->cover, "selftest: remove Cover");
    check(coverHidden, "T5 Cover 被过滤(rowOf=-1,删除零信号)");

    // Tab⑥:重建后 GeomSet 组重生
    t6Rebuild();
    check(findGroup(g_t6->M->origin, "GeomSet") != nullptr, "T6 重建后 GeomSet 组重生");

    // 全模型不变量:parent(index(c))==idx(逐 tab 逐行递归)
    struct Inv {
        static bool walk(const NavTreeModel& m, const QModelIndex& idx, int& n) {
            int rows = m.rowCount(idx);
            for (int r = 0; r < rows; ++r) {
                QModelIndex c = m.index(r, 0, idx);
                if (!c.isValid() || c.parent() != idx) return false;
                ++n;
                if (!walk(m, c, n)) return false;
            }
            return true;
        }
    };
    int n5 = 0, n6 = 0;
    check(Inv::walk(*g_t5->model, QModelIndex(), n5), "T5 模型不变量");
    check(Inv::walk(*g_t6->model, QModelIndex(), n6), "T6 模型不变量");
    printf(fails ? "== selftest FAIL(%d) ==\n" : "== selftest PASS ==\n", fails);
    return fails ? 1 : 0;
}

// ═══════════════ main ═══════════════

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    const bool selftest = app.arguments().contains(QStringLiteral("--selftest"));
    int rc = 0;

    {   // 窗口作用域:视图/模型全部先于数据析构
        QTabWidget tabs;
        tabs.setWindowTitle(QStringLiteral("NavTreeGroup · 投影引擎场景全集(tree_tabs)"));
        tabs.resize(1000, 700);

        // Tab① 透传:不装任何投影,引擎原样转发虚接口(三政体混合树)
        auto* M1 = new Machine(buildMachine("Machine-T1", false));
        tabs.addTab(simpleTab(QStringLiteral(
            "Tab① 透传原始树:未安装任何投影。数据侧三种所有权政体(m_children 连坐 / QObject 级联 / "
            "DataObject 自管)的枚举差异被虚函数吸收,引擎原样转发 —— 这是所有投影的基线。"), M1->root),
            QStringLiteral("① 透传"));

        // Tab② 按类型分组(dynamicAll:新节点自动进组)
        auto* M2 = new Machine(buildMachine("Machine-T2", false));
        P.install(M2->root, ProjectionSpec{ nullptr,
            GroupRules::byKey([](GuiBase* n) { return n->type(); },
                              [](const std::string& k) { return k + "s"; }),
            demoLess, { false, true } });
        tabs.addTab(simpleTab(QStringLiteral(
            "Tab② 按类型分组(dynamicAll=true):参与集=当前全部活孩子,数据侧新增节点下次查询自动进组。"
            "组不存成员 —— 成员关系=规则×活列表现场解析,构造上无悬空指针。"), M2->root),
            QStringLiteral("② 类型分组"));

        // Tab③ 过滤 + 材料分组
        auto* M3 = new Machine(buildMachine("Machine-T3", true));
        P.install(M3->root, ProjectionSpec{ notSuppressed(), byMaterial(), demoLess, { false, true } });
        tabs.addTab(simpleTab(QStringLiteral(
            "Tab③ 完整流水线:过滤(Cover 被压制 → 移出投影,rowOf=-1)→ 分组(零件按材料)→ 排序(demoLess:"
            "组前置,其余按类型秩+名字)。顺序有语义:过滤在分组前(隐藏件不占成员位)。"), M3->root),
            QStringLiteral("③ 过滤+材料分组"));

        // Tab④ 多层分组:root 按类型 → Part 组内再按材料(三层树)
        auto* M4 = new Machine(buildMachine("Machine-T4", false));
        P.install(M4->root, ProjectionSpec{ nullptr,
            GroupRules::byKey([](GuiBase* n) { return n->type(); },
                              [](const std::string& k) { return k + "s"; }),
            demoLess, { false, true } });
        if (TreeGroup* parts = findGroup(M4->root, "Part"))
            P.install(parts, ProjectionSpec{ nullptr, byMaterial(), demoLess, { false, true } });
        tabs.addTab(simpleTab(QStringLiteral(
            "Tab④ 多层分组(组内分组,三层):root 按类型分组后,再对 Parts 组安装材料分组。"
            "childCount 优先查节点自身投影;parentOf 从自然父亲逐层下钻 deepest-wins;淘汰组时级联拆内层投影。"),
            M4->root), QStringLiteral("④ 多层分组"));

        tabs.addTab(buildTab5(), QStringLiteral("⑤ 精准删除"));
        tabs.addTab(buildTab6(), QStringLiteral("⑥ 全量重建"));
        tabs.addTab(buildTab7(), QStringLiteral("⑦ 流水线对比"));

        // 数据清理登记(窗口析构后执行)
        g_extraCleanups.push_back([M1] { destroyMachine(*M1); delete M1; });
        g_extraCleanups.push_back([M2] { destroyMachine(*M2); delete M2; });
        g_extraCleanups.push_back([M3] { destroyMachine(*M3); delete M3; });
        g_extraCleanups.push_back([M4] { destroyMachine(*M4); delete M4; });
        g_extraCleanups.push_back([] {
            if (g_t5) { P.unbindSpec("tabs-origin-geomspec"); }   // 先解绑再拆数据
            destroyMachine(*g_t5->M); delete g_t5->M; delete g_t5; g_t5 = nullptr;
            destroyMachine(*g_t6->M); delete g_t6->M; delete g_t6; g_t6 = nullptr;
        });

        if (selftest)
            QTimer::singleShot(0, [&] { app.exit(runSelfTest()); });
        tabs.show();
        rc = app.exec();
    }   // ← 窗口/视图/模型在此析构(先于数据)

    for (auto& c : g_extraCleanups) c();              // 数据按政体清理
    printf("engine live groups after cleanup = %d\n", (int)P.liveGroupCount());
    return rc;
}
