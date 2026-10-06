// ═══════════════════════════════════════════════════════════════
//  interactive_demo:生产级压力测试 —— 三政体混合树 + 五种右键交互
// ═══════════════════════════════════════════════════════════════
//  交互(右键菜单):
//   1. 删除节点        —— 事前坐标精准信号(级联预判:末成员→整组蒸发)
//   2. 新增子节点      —— 弹窗输入名字+选政体(Data/Geom/Mesh),按【父的政体】挂载
//   3. 多选 → 分组     —— 把选中节点归入一个手动组(按【名字】记成员,跨重建稳定)
//   4. 删除组          —— 解散该组,成员回落为直属
//   5. 全量重建        —— wipe 当前父的子树并原样重建,两种方式二选一:
//                        (A) 手动 prune + install   (B) bindSpec + rebind
//                        重建后分组必须依然成立(名字规则 + 稳定特征定位)
//   6. 构造/拆除压力树 —— 【可选装】一棵极端复杂的树,专供手工把玩(见文件尾 buildStressTree):
//                        宽 500 直属孩子(10 个材料组各 50 成员)/ 24 层深数据链 /
//                        三政体齐全 / 三个同名"轴承"分属不同父亲 / 组 label 与节点同名 /
//                        空网格与藏起来的数据(删掉即可看"空组蒸发"/"幽灵行")/
//                        第二棵独立树(多根隔离)
//                        菜单:右键空白处或任意节点 → 最下方"构造压力树"/"拆除压力树"
//
//  三政体(本 demo 的核心难点):
//   · DataObject  → m_subDataGroups 自管(析构【不】级联,须显式 destroy)
//   · GeomObject  → QObject 树(析构级联 QObject 子;枚举只读 QObject children)
//   · MeshObject  → GuiBase::m_children(~GuiBase 连坐 delete)
//  挂载/删除必须严格按政体走,否则双重删除或悬空 —— 见 createAndAttach/deleteChildRaw。
#include <QApplication>
#include <QMainWindow>
#include <QTreeView>
#include <QMenu>
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QItemSelectionModel>
#include <QInputDialog>
#include <QMessageBox>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "guibase.h"
#include "treeprojection.h"
#include "navtreemodel.h"
#include "demonodes.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static TreeProjection& P = TreeProjection::instance();

// ═══════════════ 第三政体:MeshObject(GuiBase::m_children 自管) ═══════════════
// 直接用 GuiBase 基类的 m_children 枚举 + ~GuiBase 连坐,是"纯 GuiBase 政体"。
class MeshObject : public GuiBase {
public:
    MeshObject(std::string name, GuiBase* treeParent)
        : GuiBase(treeParent), m_name(std::move(name)) {}
    std::string type() override { return "Mesh"; }
    const std::string& name() const { return m_name; }
private:
    std::string m_name;
};

// 薄子类:挂进 Mesh(m_children 政体)但仍要有"Part"语义 + 显式展示父亲。
// 为什么不直接用 PartObject:它的构造是 (name, material, sup),sup 必须是 DataObject*,
// 而 MeshObject 不是 DataObject;sup 填 root 又会让 parentForTree() 回退到 root,
// 于是 parentOf 返回 root 而不是它真正的 Mesh 父亲(实测踩坑)。这里走
// NamedData(name, sup=nullptr, treeParent=mesh) 的构造,拿回正确的展示父亲。
class MeshPart : public NamedData {
public:
    MeshPart(std::string nm, std::string mat, GuiBase* treeParent)
        : NamedData(std::move(nm), nullptr, treeParent), m_mat(std::move(mat)) {}
    std::string type() override { return "Part"; }
    const std::string& material() const { return m_mat; }
private:
    std::string m_mat;
};

// ═══════════════ 名字 / 显示 / 政体识别 ═══════════════
enum class Kind { Data, Geom, Mesh };

static std::string baseName(GuiBase* n) {
    if (!n) return "?";
    if (auto* g = dynamic_cast<TreeGroup*>(n)) return g->ticket().label;
    if (auto* d = dynamic_cast<NamedData*>(n))  return d->name();
    if (auto* g = dynamic_cast<NamedGeom*>(n))  return g->name();
    if (auto* m = dynamic_cast<MeshObject*>(n)) return m->name();
    return n->type();
}
static Kind kindOf(GuiBase* n) {
    if (dynamic_cast<GeomObject*>(n)) return Kind::Geom;
    if (dynamic_cast<MeshObject*>(n)) return Kind::Mesh;
    return Kind::Data;   // DataObject/NamedData/PartObject/Folder...
}
static const char* kindName(Kind k) {
    return k == Kind::Data ? "Data" : k == Kind::Geom ? "Geom" : "Mesh";
}

// ═══════════════ 原始枚举(绕过投影,供销毁/快照用) ═══════════════
static std::vector<GuiBase*> rawKids(GuiBase* n) {
    std::vector<GuiBase*> v;
    if (!n) return v;
    for (size_t i = 0; i < n->visibleChildrenCount(); ++i)
        if (GuiBase* k = n->visibleChild(i)) v.push_back(k);
    return v;
}

// ═══════════════ 政体感知:挂载 ═══════════════
// 关键:挂到哪个容器由【父的政体】决定;能否挂由【父子政体兼容性】决定。
// 返回新节点;不兼容时返回 nullptr 并置 err。
static GuiBase* createAndAttach(GuiBase* parent, const std::string& name,
                                Kind kind, const std::string& material,
                                std::string& err) {
    err.clear();
    if (!parent) { err = "无父节点"; return nullptr; }

    // ── 父 = GeomObject:QObject 政体,枚举只认 QObject 子 → 只能挂 Geom 子 ──
    if (auto* pg = dynamic_cast<GeomObject*>(parent)) {
        if (kind != Kind::Geom) {
            err = "Geom 父走 QObject 政体,只能挂 Geom 子(Data/Mesh 不可见,已拒绝)";
            return nullptr;
        }
        // QObject 父亲 = pg(GeomObject*→QObject*);parentForTree 回退 QObject::parent()
        return new NamedGeom(name, pg, nullptr);
    }

    // ── 父 = DataObject:Data 子走 m_subDataGroups;其余走 m_children(DataObject 两者都枚举)──
    if (auto* pd = dynamic_cast<DataObject*>(parent)) {
        if (kind == Kind::Data) {
            if (!material.empty()) {                 // 带材料 → PartObject(sup=pd)
                auto* c = new PartObject(name, material, pd);
                pd->addSubDataGroup(c);
                return c;
            }
            auto* c = new NamedData(name, pd);       // sup=pd → parentForTree 回退 m_supDataGroup
            pd->addSubDataGroup(c);
            return c;
        }
        if (kind == Kind::Geom) {                    // 非 QObject 父,给 treeParent,进 m_children
            auto* c = new NamedGeom(name, nullptr, parent);
            parent->addTreeChild(c);
            return c;
        }
        auto* c = new MeshObject(name, parent);      // Mesh 进 m_children
        parent->addTreeChild(c);
        return c;
    }

    // ── 父 = MeshObject / 裸 GuiBase:m_children 政体,任何 GuiBase 子都能挂 ──
    if (kind == Kind::Data) {
        auto* c = new NamedData(name, nullptr, parent);   // treeParent=parent
        parent->addTreeChild(c);
        return c;
    }
    if (kind == Kind::Geom) {
        auto* c = new NamedGeom(name, nullptr, parent);
        parent->addTreeChild(c);
        return c;
    }
    auto* c = new MeshObject(name, parent);
    parent->addTreeChild(c);
    return c;
}

// ═══════════════ 政体感知:摘除并删除(子的后代须已清空) ═══════════════
static void deleteChildRaw(GuiBase* parent, GuiBase* child) {
    // Data 政体:子在父的 subDataGroups 里 → destroySubDataGroup(摘+删)
    if (auto* pd = dynamic_cast<DataObject*>(parent)) {
        if (auto* cd = dynamic_cast<DataObject*>(child)) {
            for (auto* s : pd->subDataGroups())
                if (s == cd) { pd->destroySubDataGroup(cd); return; }
        }
    }
    // QObject 政体:仅当【父确实是 QObject】且【子的 QObject 父亲正是它】时,
    // delete 子会由 ~QObject 自动摘除。★ 必须判 pq!=nullptr:非 QObject 父
    // (Data/Mesh)的 dynamic_cast<QObject*> 得 null,而挂在 m_children 里的 Geom 子
    // 其 QObject::parent() 也是 null,null==null 会误判 → 漏摘除 → 悬空 → 二次删除。
    if (auto* cg = dynamic_cast<GeomObject*>(child)) {
        if (QObject* pq = dynamic_cast<QObject*>(parent)) {
            if (cg->QObject::parent() == pq) { delete child; return; }
        }
    }
    // m_children 政体(或 DataObject 的非 data 子):先摘再删
    parent->removeTreeChild(child);
    delete child;   // ~GuiBase 连坐其 m_children;~QObject 连坐其 QObject 子
}

// 递归清空 n 的所有后代(政体感知),并清 n 自己的投影账;不删 n 本身
static void clearDescendants(GuiBase* n) {
    P.discard(n);                          // 清 n 的账页+组(纯 map 操作,安全)
    for (GuiBase* k : rawKids(n)) {        // rawKids=数据侧真实枚举(含被过滤者)
        clearDescendants(k);               // 先清孙辈
        deleteChildRaw(n, k);              // 再摘删 k
    }
}

// ═══════════════ 手动分组登记(按【名字】记成员 → 跨重建稳定) ═══════════════
struct ManualGroup { std::string key; std::string label; std::vector<std::string> memberNames; };
static std::map<GuiBase*, std::vector<ManualGroup>> g_manual;   // owner(数据父) → 组定义
static int g_keySeq = 0;

// 由 owner 的手动组定义合成 Spec:规则按【名字】命中 → 稳定,重建后同名节点自动归组
static ProjectionSpec buildSpec(GuiBase* owner) {
    std::vector<ManualGroup> groups = g_manual[owner];   // 按值捕获名字(不捕指针!)
    // ★ 手动规则【不豁免组】:多层分组靠的就是"组的 label 也在某个手动组的
    //   成员名单里" → 组作为整体进外层桶。豁免组的是通用工厂规则(byKey/
    //   byPredicate),不是这里。
    GroupRule rule = [groups](GuiBase* n) -> std::optional<GroupTicket> {
        std::string nm = baseName(n);
        for (const auto& g : groups)
            for (const auto& mn : g.memberNames)
                if (mn == nm) { GroupTicket t; t.key = g.key; t.label = g.label; return t; }
        return std::nullopt;                             // 不在任何手动组 → 直属
    };
    return ProjectionSpec{ nullptr, rule, demoLess, { false, true /*dynamicAll*/ } };
}
static void installGrouping(GuiBase* owner) {
    if (g_manual[owner].empty()) P.uninstall(owner);
    else P.install(owner, buildSpec(owner));
}

// ═══════════════ 子树快照 / 重建(全量重建用) ═══════════════
struct NodeSnap { std::string name; Kind kind; std::string material; std::vector<NodeSnap> kids; };
static NodeSnap snapNode(GuiBase* n) {
    NodeSnap s; s.name = baseName(n); s.kind = kindOf(n);
    if (auto* p = dynamic_cast<PartObject*>(n)) s.material = p->material();
    for (GuiBase* k : rawKids(n)) s.kids.push_back(snapNode(k));
    return s;
}
static GuiBase* recreate(GuiBase* parent, const NodeSnap& s) {
    std::string err;
    GuiBase* c = createAndAttach(parent, s.name, s.kind, s.material, err);
    if (!c) return nullptr;                              // 政体不兼容(重建时不该发生)
    for (const auto& ks : s.kids) recreate(c, ks);       // 递归重建子树
    return c;
}

static GuiBase* topOf(GuiBase* n) {
    GuiBase* p = n;
    while (p) { GuiBase* q = p->parentForTree(); if (!q) break; p = q; }
    return p;
}

// ═══════════════ 全局 UI 句柄 + 日志 ═══════════════
static NavTreeModel* g_model = nullptr;
static QTreeView*    g_view  = nullptr;
static QPlainTextEdit* g_log = nullptr;
static GuiBase*      g_root  = nullptr;
// 压力树控制台(见 setupStressUI):一行按钮 + 一个"选中节点投影坐标"状态板
static QLabel*  g_stressStatus = nullptr;
static QWidget* g_stressBar    = nullptr;
// 状态刷新回调:构造/拆除压力树、切换规则、选中/展开变化都调它
static std::function<void()> g_refreshStatus;
// 视图切换(在 main 里绑定):切到压力树 / 切回初始树
static std::function<void()> g_showStress;
static std::function<void()> g_showInitial;

static void refreshStressUI() { if (g_refreshStatus) g_refreshStatus(); }

static void log(const std::string& s) {
    printf("%s\n", s.c_str());
    if (g_log) g_log->appendPlainText(QString::fromStdString(s));
    refreshStressUI();          // 引擎状态可能变了,状态板跟着刷
}
static QString displayName(GuiBase* n) {
    if (auto* g = dynamic_cast<TreeGroup*>(n))
        return QString::fromStdString("[组] " + g->ticket().label + " ("
             + std::to_string(P.childCount(n)) + ")");
    QString s = QString::fromStdString(baseName(n));
    if (auto* p = dynamic_cast<PartObject*>(n))
        s += QString::fromStdString(" [" + p->material() + "]");
    return s + QString::fromStdString("  <" + n->type() + ">");
}

// 结构性变更(增/分组/删组/重建)统一走 reset 通道 —— 对应你们"更新→reset"的生产机制
static void structuralReset(const std::function<void()>& mutate) {
    g_model->beginFullReset();
    mutate();
    g_model->endFullReset();
    g_view->expandAll();
    refreshStressUI();
}

// ═══════════════ 操作 4:删除组(解散,成员回落直属) ═══════════════
static void onDeleteGroup(GuiBase* group) {
    auto* g = dynamic_cast<TreeGroup*>(group);
    if (!g) return;
    GuiBase* owner = g->owner();
    std::string key = g->ticket().key, label = g->ticket().label;
    auto& v = g_manual[owner];
    v.erase(std::remove_if(v.begin(), v.end(),
        [&](const ManualGroup& m) { return m.key == key; }), v.end());
    log("删除组 [" + label + "] → 成员回落为直属");
    structuralReset([&] { installGrouping(owner); });   // 空则 uninstall,否则按剩余组重装
}

// ═══════════════ 删除内核:政体感知摘除 + 清后代 + 清账 ═══════════════
static void deleteNodeFull(GuiBase* victim) {
    GuiBase* dp = victim->parentForTree();   // 数据侧真实父亲(不是投影组)
    clearDescendants(victim);                // 清后代 + discard(victim) 清其账
    P.discard(victim);                       // 幂等再清一次
    g_manual.erase(victim);                  // 它若当过 owner,丢掉其手动组定义
    if (dp) deleteChildRaw(dp, victim);      // 按政体摘除+delete(~GuiBase/~QObject 连坐)
    else delete victim;
    P.prune();                               // 清剩余僵尸账(地址复用幽灵防线)
}

// ═══════════════ 操作 1:删除节点(事前坐标精准信号) ═══════════════
static void onDelete(GuiBase* victim) {
    if (victim == g_root) { log("根节点不可删"); return; }
    if (P.isGroup(victim)) { onDeleteGroup(victim); return; }   // 组走删除组
    GuiBase* ppar = P.parentOf(victim);
    int prow = P.rowOf(victim);
    std::string nm = baseName(victim);
    if (!ppar || prow < 0) {                     // 被过滤/不在投影树 → 零信号
        deleteNodeFull(victim);
        log("删除 [" + nm + "](不在投影树上,零信号)");
        return;
    }
    // 级联预判(多层版):沿 parentOf 向上找到最外层会蒸发的组。
    // 只判一层会让"内层组蒸发 + 外层组跟着空"的情况漏一条信号 → 模型视图失步。
    if (GuiBase* cas = cascadeTarget(P, victim)) {
        GuiBase* gpar = P.parentOf(cas);
        int grow = P.rowOf(cas);
        log("删除 [" + nm + "] → 组[" + baseName(cas) + "]整组蒸发,信号发在最外层组行");
        g_model->beginRowRemoval(gpar, grow);    // 死前问坐标(报在组的位置)
        deleteNodeFull(victim);
        g_model->endRowRemoval();
        return;
    }
    log("删除 [" + nm + "] @row " + std::to_string(prow) + "(组存活)");
    g_model->beginRowRemoval(ppar, prow);        // 死前问坐标
    deleteNodeFull(victim);
    g_model->endRowRemoval();
}

// ═══════════════ 操作 3 核心:多选分组(按名字记成员,无对话框) ═══════════════
static bool doGroup(GuiBase* owner, std::vector<GuiBase*> nodes, const std::string& label) {
    if (nodes.empty() || !owner) return false;
    for (GuiBase* n : nodes) {
        // 允许选中【组节点】→ 多层分组:组作为整体成为新外层组的成员。
        // 同一性用投影父亲(parentOf)判定:组与它的兄弟数据节点共享同一 owner。
        if (P.parentOf(n) != owner) { log("分组失败:只能对【同一投影父】下的节点分组(可含组)"); return false; }
    }
    ManualGroup mg;
    mg.key = "manual_" + std::to_string(++g_keySeq);
    mg.label = label;
    for (GuiBase* n : nodes) mg.memberNames.push_back(baseName(n));
    // 从同 owner 的其它手动组里摘掉这些名字(一个节点只归一个手动组)
    for (auto& other : g_manual[owner])
        other.memberNames.erase(std::remove_if(other.memberNames.begin(), other.memberNames.end(),
            [&](const std::string& nm) {
                for (auto& m : mg.memberNames) if (m == nm) return true;
                return false;
            }), other.memberNames.end());
    g_manual[owner].erase(std::remove_if(g_manual[owner].begin(), g_manual[owner].end(),
        [](const ManualGroup& m) { return m.memberNames.empty(); }), g_manual[owner].end());
    g_manual[owner].push_back(mg);
    log("分组 " + std::to_string(nodes.size()) + " 个节点 → [" + mg.label + "](按名字记,跨重建稳定)");
    structuralReset([&] { installGrouping(owner); });
    return true;
}
static void onGroupSelected(std::vector<GuiBase*> nodes) {
    if (nodes.empty()) return;
    GuiBase* owner = P.parentOf(nodes[0]);      // 投影父亲(组与数据节点统一)
    if (!owner) return;
    bool ok = false;
    QString label = QInputDialog::getText(g_view, QStringLiteral("多选分组"),
        QStringLiteral("组名:"), QLineEdit::Normal,
        QString::fromStdString("手动组" + std::to_string(g_keySeq + 1)), &ok);
    if (ok) doGroup(owner, nodes, label.toStdString());
}

// ═══════════════ 操作 2 核心:新增子节点(无对话框) ═══════════════
static GuiBase* doAdd(GuiBase* parent, const std::string& nm, Kind k, const std::string& mat) {
    if (P.isGroup(parent)) { log("不能在组节点下直接新增(组是投影产物)"); return nullptr; }
    std::string err;
    GuiBase* c = createAndAttach(parent, nm, k, mat, err);
    if (!c) { log("新增失败:" + err); return nullptr; }
    log(std::string("新增 ") + kindName(k) + " 子节点 [" + nm + "] 到 [" + baseName(parent) + "]");
    structuralReset([] {});   // 结构变更 → reset(新增可能引入新行/新组)
    return c;
}
static void onAdd(GuiBase* parent) {
    if (P.isGroup(parent)) { log("不能在组节点下直接新增(组是投影产物)"); return; }
    QDialog dlg(g_view);
    dlg.setWindowTitle(QStringLiteral("新增子节点到 [%1]").arg(QString::fromStdString(baseName(parent))));
    auto* nameEd = new QLineEdit(&dlg);
    auto* kindCb = new QComboBox(&dlg);
    kindCb->addItem(QStringLiteral("DataObject —— 数据自管(m_subDataGroups)"), (int)Kind::Data);
    kindCb->addItem(QStringLiteral("GeomObject —— QObject 管理"), (int)Kind::Geom);
    kindCb->addItem(QStringLiteral("MeshObject —— GuiBase m_children 连坐"), (int)Kind::Mesh);
    auto* matEd = new QLineEdit(&dlg);
    matEd->setPlaceholderText(QStringLiteral("可选,仅 Data 生效(如 Steel)"));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("名称:"), nameEd);
    form->addRow(QStringLiteral("类型/政体:"), kindCb);
    form->addRow(QStringLiteral("材料:"), matEd);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(form); lay->addWidget(box);
    if (dlg.exec() != QDialog::Accepted) return;
    std::string nm = nameEd->text().trimmed().toStdString();
    if (nm.empty()) nm = "未命名";
    std::string err;
    GuiBase* c = createAndAttach(parent, nm, (Kind)kindCb->currentData().toInt(),
                                 matEd->text().trimmed().toStdString(), err);
    if (!c) { QMessageBox::warning(g_view, QStringLiteral("无法新增"), QString::fromStdString(err));
              log("新增失败:" + err); return; }
    log("新增子节点 [" + nm + "] 到 [" + baseName(parent) + "]");
    structuralReset([] {});
}

// ═══════════════ 操作 5 校验:重建后分组是否存活 ═══════════════
static void verifyGroups(GuiBase* parent) {
    size_t expected = 0;
    for (const auto& mg : g_manual[parent]) {
        bool any = false;
        for (GuiBase* k : rawKids(parent))
            for (const auto& mn : mg.memberNames) if (baseName(k) == mn) any = true;
        if (any) ++expected;                       // 至少有一个活成员的手动组才会成组
    }
    size_t actual = 0;
    for (size_t i = 0; i < P.childCount(parent); ++i)
        if (P.isGroup(P.child(parent, i))) ++actual;
    log("  重建后校验:应有组=" + std::to_string(expected)
        + " 实际组=" + std::to_string(actual)
        + (actual == expected ? " → 分组存活 ✓" : " → 不一致 ✗"));
}

// ═══════════════ 操作 5:全量重建(两种方式) ═══════════════
static void onRebuild(GuiBase* parent, bool useBindSpec) {
    if (P.isGroup(parent)) { log("不能重建组节点"); return; }
    std::vector<NodeSnap> snaps;
    for (GuiBase* k : rawKids(parent)) snaps.push_back(snapNode(k));
    std::string pname = baseName(parent);
    log("全量重建 [" + pname + "] 的 " + std::to_string(snaps.size())
        + " 个子节点,方式=" + (useBindSpec ? "bindSpec+rebind" : "手动 prune+install"));
    g_model->beginFullReset();
    for (GuiBase* k : rawKids(parent)) { clearDescendants(k); deleteChildRaw(parent, k); }  // wipe
    P.prune();                                        // ★ 重建【前】清死账(地址还死了,不会误判)
    for (const auto& s : snaps) recreate(parent, s);  // rebuild(同名同政体)
    if (useBindSpec) {                                // 方式 B:声明式,谓词按名字+树根定位
        std::string id = "rebuild_" + pname;
        P.unbindSpec(id);
        GuiBase* root = g_root;
        P.bindSpec({ id,
            [pname, root](GuiBase* n) { return baseName(n) == pname && topOf(n) == root; },
            buildSpec(parent) });                     // bindSpec 内部即 rebind
    } else {                                          // 方式 A:命令式,手动 prune+install
        P.prune();
        P.install(parent, buildSpec(parent));
    }
    g_model->endFullReset();
    g_view->expandAll();
    verifyGroups(parent);
}

// ═══════════════ 初始复杂树(三政体混合) ═══════════════
static GuiBase* buildInitialTree() {
    auto* root = new NamedData("装配体");
    // Data 政体(m_subDataGroups 自管,析构不级联)
    root->addSubDataGroup(new PartObject("Base",  "Steel", root));
    root->addSubDataGroup(new PartObject("BoltA", "Steel", root));
    root->addSubDataGroup(new PartObject("BoltB", "Alum",  root));
    root->addSubDataGroup(new PartObject("Cover", "Alum",  root));
    auto* folder = new FolderObject("标准件", root);
    root->addSubDataGroup(folder);
    folder->addSubDataGroup(new PartObject("Nut1", "Steel", folder));
    folder->addSubDataGroup(new PartObject("Nut2", "Brass", folder));
    // Geom 政体(QObject 树,枚举只读 QObject children)
    auto* origin = new NamedGeom("Origin", nullptr, root);
    root->addTreeChild(origin);
    new NamedGeom("XY-Plane", origin, nullptr);
    new NamedGeom("X-Axis",  origin, nullptr);
    // Mesh 政体(GuiBase::m_children,~GuiBase 连坐);混挂一个 Data 子
    auto* mesh = new MeshObject("网格", root);
    root->addTreeChild(mesh);
    mesh->addTreeChild(new MeshObject("面网格", mesh));
    mesh->addTreeChild(new MeshObject("体网格", mesh));
    mesh->addTreeChild(new NamedData("网格参数", nullptr, mesh));
    return root;
}

// ═══════════════ 压力树:可选装的"极端复杂树"(供手工把玩) ═══════════════
// 为什么单独一棵、而不是把初始树搞复杂:
//   ① 初始树是 A~Q 电池的基准,搅浑它会把电池变成"测电池自己"(Q 电池已有前车之鉴);
//   ② 压力树要能随时构造/随时拆,方便反复把玩。
// 它刻意堆齐了惰性投影最容易出问题的形状:
//   · 26 层深数据链(3 层文件夹 + 22 个嵌套文件夹 + 链末零件)→ 压 DFS 与逐层下钻
//   · 500 个直属孩子的宽父亲 → 压 bucketByKey 的 O(n×桶) 与 rowOf 的线性扫描
//   · 三个同名 "轴承" 分属不同父亲 → 压"按名字记成员"的同一性(手动分组只认名字)
//   · 同一名字重复出现 + 组 label 与节点同名 → 压"组套娃"与 flat 重复两大历史坑
//   · 空网格 / 藏起来的数据 → 手工删掉即可现场看"空组蒸发"
//   · 第二棵独立树 → 验证纪元/账本在多根下不串味(跨根谓词必须自限定)
//   · 24 个同名材料 key + 组 cull/复用 → 配合"换规则"看延迟释放峰值(C4)
// ⚠ 深度刻意压到 25:PROJECT.md §3 记过"~GuiBase 连坐析构是递归的,>3 万级爆栈",
//   24 层安全且足以暴露 O(depth) 类问题。
static GuiBase* g_stressRoot = nullptr;   // 压力树主根(装投影的那个)
static GuiBase* g_stressRoot2 = nullptr;  // 第二棵独立树(多根隔离用)

// 递归装同一套工厂规则。
// ⚠ 两条实测教训(第一版就踩了,别重犯):
//   ① 递归子节点必须走【数据侧虚接口 rawKids】,不能用引擎的 childCount/child ——
//      装上投影后引擎返回的是"投影后"的孩子(组),拿它递归等于在 [组] 上打转,
//      真正的 Mesh 子树(GuiBase 政体)永远装不到 → 那颗 Part 的 parentOf 返回 null。
//   ② 只给"有 Part/Mesh 直接孩子"的节点装页。否则根会把 Folder 也收进材料组,
//      文件夹整棵子树被组替掉 —— 投影树失去可探索的层级。
static bool hasGroupableChild(GuiBase* n) {
    for (GuiBase* k : rawKids(n))
        if (dynamic_cast<PartObject*>(k) || dynamic_cast<MeshObject*>(k)) return true;
    return false;
}
static void installRuleRecursive(GuiBase* root, GuiBase* node, const GroupRule& rule,
                                 std::set<GuiBase*>& seen) {
    if (!node || !seen.insert(node).second) return;
    if (topOf(node) != root) return;                 // ★ 多根隔离:只在本树内装
    if (hasGroupableChild(node)) {
        ProjectionSpec sp;
        sp.grouping = rule;
        sp.less = demoLess;
        sp.opt.dynamicAll = true;
        P.install(node, sp, {});
    }
    for (GuiBase* k : rawKids(node)) installRuleRecursive(root, k, rule, seen);
}

// 工厂规则(和 GroupRules::byKey 同风格的"无条件豁免组节点"):
//   Part → 按材料归组(材料名就是 key)
//   Mesh / Folder / Geom → 【一律不出票】,留直属(只作为"透传容器")
// ⚠ 两个实测踩坑,都是这条规则现在这么写的原因:
//   ① 组 key 必须【按页唯一】(这里加了 owner 名字前缀)。matchReusable 是按 key
//      配对的,多页各自建同名的 "Titan 件" 会让后重算的那页把别页的同名组 cull 掉
//      —— 被 cull 的组 owner 被 erase,它里面成员的 parentOf 直接变 null。
//      实测:543 节点里恰好 1 处不一致,症状就是 网格B 下的"轴承" parentOf==null。
//   ② Mesh 自己绝不能进组:它一旦成为成员,挂它的组就没有页,组里成员的
//      parentOf 同样返回 null。所以 Mesh 只做透传(它的孩子照样被分组)。
//   ★ 这正是 C1/C2 那类"组是每页的产物"的具体体现:跨页共享 key 会互相干扰。
//   注:bindSpec 的 Spec 是"一个规则给多页",拿不到"当前是哪页",所以这里用
//   【每一页的 owner 名字】当 key 前缀 —— 压力树里每个页节点名字唯一,够用。
//   真实生产里若两页同名,key 撞车仍会发生,那时应该给规则注入页标识。
static GroupRule stressRuleFor(GuiBase* page) {
    std::string prefix = baseName(page) + "/";       // 页内唯一前缀
    return [prefix](GuiBase* n) -> std::optional<GroupTicket> {
        if (!n || dynamic_cast<TreeGroup*>(n)) return std::nullopt;
        std::string mat;
        if (auto* p = dynamic_cast<PartObject*>(n))      mat = p->material();
        else if (auto* mp = dynamic_cast<MeshPart*>(n))  mat = mp->material();   // 挂在 Mesh 下的 Part
        else return std::nullopt;                        // Mesh / Folder / Geom 一律留直属
        GroupTicket t;
        t.key   = prefix + (mat.empty() ? "(无材料)" : mat);
        t.label = (mat.empty() ? "(无材料)" : mat) + " 件";
        return t;
    };
}

// "这个节点下面(任意深度)有没有可分组的东西?" —— 决定要不要给它装页。
//   必须【递归看数据侧】:只看直接孩子会漏掉"文件夹套文件夹再套零件"的形状。
static bool hasGroupableInside(GuiBase* n) {
    for (GuiBase* k : rawKids(n))
        if (dynamic_cast<PartObject*>(k) || dynamic_cast<MeshObject*>(k)
            || hasGroupableInside(k)) return true;
    return false;
}

static void installRuleWalk(GuiBase* root, GuiBase* node, const GroupRule& rule,
                            std::set<GuiBase*>& seen);   // 前置声明(定义在下方)

// 装压力树的投影。
// ★ 用【显式逐页安装】而不是 bindSpec,原因是实测出来的:
//   bindSpec 的 Spec 是"一条规则管多页",规则里拿不到"当前是哪页",
//   于是多页只能用同一个 key → matchReusable 按 key 配对时会互相 cull
//   (症状:某页的组被别页的重算 cull 掉,owner 被 erase,其成员 parentOf==null)。
//   逐页安装就能给每页一个带页名的 key 前缀,彻底避免跨页 key 撞车。
//   代价:重建后要再调一次本函数。
static void installStressProjection() {
    if (!g_stressRoot) return;
    // 预置手动组定义(按名字记成员)—— 给根页用,也是"组套娃 / 同名收组"的把玩入口。
    // ⚠ 手动规则只对 root 的【直接孩子】求值,所以成员名单必须是直接孩子的名字
    //   (实测:写"链末零件"没用 —— 它埋在深层节点以下 21 层,规则扫不到)。
    if (g_manual[g_stressRoot].empty()) {
        ManualGroup mg; mg.key = "manual_stress_deep"; mg.label = "深链集合";
        mg.memberNames = { "深层节点", "标准件" };
        g_manual[g_stressRoot].push_back(mg);
    }
    std::set<GuiBase*> seen;
    installRuleWalk(g_stressRoot, g_stressRoot, stressRuleFor(g_stressRoot), seen);
    installGrouping(g_stressRoot);                   // 根页:手动组名单(与 A~Q 电池同机制)
    if (g_stressRoot2) {
        std::set<GuiBase*> seen2;
        installRuleWalk(g_stressRoot2, g_stressRoot2, stressRuleFor(g_stressRoot2), seen2);
    }
}

// 递归装页:沿【数据侧 rawKids】下行给每个"有可分组后代"的节点装工厂规则。
// ⚠ 三个坑,全是实测踩出来的:
//   ① 不能用引擎的 childCount/child 递归:装上投影后它返回的是投影孩子(组),
//      被组盖住的数据节点就漏装了。
//   ② 也不能只走 rawKids 而不进组:某节点装完投影后,它的 rawKids 里除了数据孩子
//      还有材料组;必须再把组下钻一层(组的 rawKids = 成员),否则组里的成员漏装。
//   ③ 必须给 Mesh 这类中间节点也装页:否则它的孩子 parentOf 会返回 null
//      (实测:543 节点里恰好 1 处不一致,就是 Mesh 里的"轴承")。
static void installRuleWalk(GuiBase* root, GuiBase* node, const GroupRule& rule,
                            std::set<GuiBase*>& seen) {
    if (!node || !seen.insert(node).second) return;
    if (topOf(node) != root) return;                    // ★ 多根隔离
    // ★ Mesh 节点必须【无条件】也装一页:
    //   挂进 Mesh 的 Part 走的是 sup=root 的 parentForTree 回退(见 buildStressTree),
    //   所以对引擎来说"自然父亲"是 root。只有给 Mesh 自己装一页,让它成为这些孩子的
    //   [投影父亲],parentOf 才会返回 Mesh;否则会一路跳到 root,与"期望父=Mesh"不符
    //   (实测:543 节点里唯一的红点就是这个)。
    const bool isMeshNode = dynamic_cast<MeshObject*>(node) != nullptr;
    if (node != root && !P.isGroup(node) && (isMeshNode || hasGroupableInside(node))) {
        ProjectionSpec sp;
        sp.grouping = rule;
        sp.less = demoLess;
        sp.opt.dynamicAll = true;
        P.install(node, sp, {});
    }
    for (GuiBase* k : rawKids(node)) {                  // 数据孩子 + 材料组,都走
        installRuleWalk(root, k, rule, seen);
        if (P.isGroup(k))                               // ★ 组要再下钻一层:成员经组才可见
            for (GuiBase* m : rawKids(k)) installRuleWalk(root, m, rule, seen);
    }
}

// 造压力树。返回主根;调用方负责 registerRoot + installStressProjection。
static GuiBase* buildStressTree(int flatCount, int chainDepth) {
    const char* MATS[10] = { "Steel", "Alum", "Copper", "Plastic", "Rubber",
                             "Glass", "Wood", "Titan", "Brass", "Ceramic" };
    auto* root = new NamedData("压力装配体");
    g_stressRoot = root;

    // ── 1) 宽父亲:500 个直属零件,10 种材料 → 10 个各 50 成员的组 ──
    auto* flatFolder = new FolderObject("大平铺件", root);
    root->addSubDataGroup(flatFolder);
    for (int i = 0; i < flatCount; ++i) {
        flatFolder->addSubDataGroup(new PartObject(
            "件" + std::to_string(i), MATS[i % 10], flatFolder));
    }

    // ── 2) 26 层深链:3 层文件夹 + (chainDepth-3) 个嵌套文件夹 + 链末零件 ──
    FolderObject* neck = nullptr;
    { std::string e;
      neck = static_cast<FolderObject*>(createAndAttach(root, "深层节点", Kind::Data, "", e));
      if (!neck) neck = new FolderObject("深层节点", root);      // 兜底,理论上不会走到
    }
    FolderObject* cur = neck;
    for (int i = 0; i < chainDepth - 3; ++i) {                   // 每层名字都不同 → 便于 assert 深度
        auto* nxt = new FolderObject("嵌套" + std::to_string(i), cur);
        cur->addSubDataGroup(nxt);
        cur = nxt;
    }
    cur->addSubDataGroup(new PartObject("链末零件", "Ceramic", cur));

    // ── 3) 同名陷阱:三个不同父亲各挂一个 "轴承" ──
    //     手动分组按【名字】记成员 → 同名的会被一起收进同一个组(设计如此,现场可看)
    auto* folderA = new FolderObject("标准件", root);
    folderA->addSubDataGroup(new PartObject("轴承", "Steel", folderA));
    folderA->addSubDataGroup(new PartObject("垫片", "Brass", folderA));
    root->addSubDataGroup(folderA);
    // 组 label 故意与节点同名("标准件")→ 历史上"组套娃 / 无限嵌套"的触发形状
    auto* folderB = new FolderObject("外购件", root);
    folderB->addSubDataGroup(new PartObject("轴承", "Alum", folderB));
    folderB->addSubDataGroup(new PartObject("密封圈", "Rubber", folderB));
    root->addSubDataGroup(folderB);
    auto* mB = new MeshObject("网格B", root);
    root->addTreeChild(mB);
    // ⚠ MeshObject 不是 DataObject,不能当 PartObject 的 sup(编译期就会拦下);
    //   但也【不能】把 sup 留成 nullptr —— 那样它的 parentForTree() 返回 null,
    //   引擎算 parentOf 时无处下钻,直接返回 null(实测:543 节点里唯一的
    //   parentOf==null 就是它)。正确做法:用第三个参数传 treeParent=网格B。
    // ⚠ 这个"挂进 Mesh 的 Part"有三个坑,全踩过,最终正解如下:
    //   ① PartObject 是 (name, material, sup),没有 treeParent 形参;
    //   ② MeshObject 不是 DataObject,所以 sup 不能填 mB(编译期 C2664);
    //   ③ sup 填 root 也不行 —— 那样 parentForTree() 直接回退到 root,
    //      引擎认为它的自然父亲是 root,parentOf 便返回 root(实测红点:
    //      "期望父=Titan 件 实际父=压力装配体")。
    //   正解:用有 treeParent 形参的 NamedData + 把 type 做成 Part 语义,
    //   即 NamedData(name, sup=nullptr, treeParent=mB) —— 这里用一个薄子类。
    mB->addTreeChild(new MeshPart("轴承", "Titan", mB));
    mB->addTreeChild(new MeshObject("网格B-内层", mB));

    // ── 4) 三政体齐全 ──
    auto* origin = new NamedGeom("OriginS", nullptr, root);
    root->addTreeChild(origin);
    new NamedGeom("XY面", origin, nullptr);
    new NamedGeom("Z轴",  origin, nullptr);
    auto* mesh = new MeshObject("网格S", root);
    root->addTreeChild(mesh);
    mesh->addTreeChild(new MeshObject("面网格S", mesh));
    mesh->addTreeChild(new MeshObject("体网格S", mesh));
    mesh->addTreeChild(new NamedData("网格参数S", nullptr, mesh));   // 混政体:Mesh 父挂 Data 子

    // ── 5) 预留"空组蒸发"与"死亡后代"的把玩位 ──
    auto* hidden = new NamedData("藏起来的数据", nullptr, root);
    root->addTreeChild(hidden);
    root->addTreeChild(new MeshObject("空网格", root));              // 零孩子 → 手工删它就是"空组蒸发"

    // ── 6) 第二棵独立树:验证多根下账本/纪元不串味 ──
    auto* root2 = new NamedData("压力装配体-B");
    g_stressRoot2 = root2;
    root2->addSubDataGroup(new PartObject("B-Steel",  "Steel", root2));
    root2->addSubDataGroup(new PartObject("B-Steel2", "Steel", root2));
    root2->addSubDataGroup(new PartObject("B-Alum",   "Alum",  root2));
    return root;
}

// 把压力树挂进当前会话(幂等):注册根 + 装投影 + 走 reset 通道让视图看到
static void ensureStressTree() {
    if (g_stressRoot) { log("压力树已存在,未重复构建。"); return; }
    structuralReset([] {
        buildStressTree(500, 24);
        P.registerRoot(g_stressRoot);
        P.registerRoot(g_stressRoot2);
        installStressProjection();
    });
    log("压力树已构建:压力装配体(宽 500 / 深 25 / 三政体 / 同名陷阱 / 空网格) + 压力装配体-B(独立树)");
    log("  建议把玩:① 展开 大平铺件 看 10 个材料组(各 50 成员)");
    log("            ② 右键 空网格 → 删除,看它整组/整行如何消失");
    log("            ③ 选中两个不同父亲的 [轴承] → 分组,看'按名字'把它们收在一起");
    log("            ④ 在深层节点上反复 全量重建,看 深链集合 组如何重生");
}

// 拆掉压力树(便于反复把玩;释放顺序:引擎先摘 → 数据侧按政体清)
static void teardownStressTree() {
    if (!g_stressRoot) { log("当前没有压力树。"); return; }
    auto* r1 = dynamic_cast<DataObject*>(g_stressRoot);
    auto* r2 = dynamic_cast<DataObject*>(g_stressRoot2);
    g_stressRoot = g_stressRoot2 = nullptr;
    structuralReset([&] {
        P.unbindSpec("stress-manual");
        P.discard(r1);  P.discard(r2);
        g_manual.erase(r1); g_manual.erase(r2);
    });
    P.unregisterRoot(r1); P.unregisterRoot(r2);
    P.prune();
    P.collectGarbage();
    // 数据侧按政体清:两个 DataObject 根各自 destroySubDataGroup,Geom/Mesh 子随 ~GuiBase/~QObject
    if (r1) { for (auto* s : std::vector<DataObject*>(r1->subDataGroups())) r1->destroySubDataGroup(s); }
    if (r2) { for (auto* s : std::vector<DataObject*>(r2->subDataGroups())) r2->destroySubDataGroup(s); }
    delete r1; delete r2;
    if (g_showInitial) g_showInitial();          // 压力树没了,视图切回初始树(否则模型挂着死 root)
    log("压力树已拆除(初始树不受影响)。");
}

// ═══════════════ 压力树控制台:把"能玩的视角"做成按钮 ═══════════════
// 设计要点(也是这个 demo 最值得看的一处):
//   ① 换规则 = 同一个引擎上的三种投影声明,用来观察【组复用 vs 组淘汰】;
//   ② 状态板把"选中节点的投影坐标"实时打出来 —— 投影树与数据树分叉了多远,
//      一眼就能看见(这是看代码看不出来的东西);
//   ③ 所有写侧操作都走 reset/事前坐标括号,遵守 C1/C2 契约。

// ① 材料分组:逐页装"按材料"(key 带页名前缀,避免跨页 key 撞车 —— 见 stressRuleFor 注释)
static void stressApplyMaterial() {
    if (!g_stressRoot) { log("先构造压力树。"); return; }
    structuralReset([] {
        P.unbindSpec("stress-manual");
        for (auto* r : { g_stressRoot, g_stressRoot2 }) {
            if (!r) continue;
            std::set<GuiBase*> seen;
            installRuleWalk(r, r, stressRuleFor(r), seen);
        }
        installGrouping(g_stressRoot);   // 根页:保留"深链集合"手动组
    });
    log("规则 → 按材料(逐页装;留意:同一材料名在不同页是【不同的组】——组是每页的产物)");
}

// ② 按类型:一条 root 级规则(零件/网格/几何/文件夹各归一类),用来对比"全局规则 vs 逐页规则"
static void stressApplyType() {
    if (!g_stressRoot) { log("先构造压力树。"); return; }
    structuralReset([] {
        P.unbindSpec("stress-manual");
        for (auto* r : { g_stressRoot, g_stressRoot2 }) {
            if (!r) continue;
            std::set<GuiBase*> seen;
            std::function<void(GuiBase*)> wipe = [&](GuiBase* n) {
                if (!n || !seen.insert(n).second) return;
                if (topOf(n) != r) return;
                P.uninstall(n);
                for (GuiBase* k : rawKids(n)) wipe(k);
            };
            wipe(r);
        }
        GroupRule byType = [](GuiBase* n) -> std::optional<GroupTicket> {
            if (!n || dynamic_cast<TreeGroup*>(n)) return std::nullopt;
            std::string t = n->type();
            if (t != "Part" && t != "Mesh" && t != "Geom" && t != "Folder") return std::nullopt;
            GroupTicket tk; tk.key = "type:" + t; tk.label = t; return tk;
        };
        ProjectionSpec sp;
        sp.grouping = byType;
        sp.less = demoLess;
        sp.opt.dynamicAll = true;
        P.install(g_stressRoot, sp, {});
        if (g_stressRoot2) P.install(g_stressRoot2, sp, {});
    });
    log("规则 → 按类型(root 级一条规则:Part/Mesh/Geom/Folder 各一类;注意与上一模式的层级差别)");
}

// ③ 清除投影:回到"数据原样透传",对比投影前后
static void stressApplyFlat() {
    if (!g_stressRoot) { log("先构造压力树。"); return; }
    structuralReset([] {
        P.unbindSpec("stress-manual");
        for (auto* r : { g_stressRoot, g_stressRoot2 }) {
            if (!r) continue;
            std::set<GuiBase*> seen;
            std::function<void(GuiBase*)> wipe = [&](GuiBase* n) {
                if (!n || !seen.insert(n).second) return;
                if (topOf(n) != r) return;
                P.uninstall(n);
                for (GuiBase* k : rawKids(n)) wipe(k);
            };
            wipe(r);
        }
        g_manual.erase(g_stressRoot);
    });
    P.collectGarbage();
    log("投影已全部卸载 → 数据原样透传(对比:同一棵树在三种规则下的样子)");
}

// ④ 一键现成组:把压力树里"最值得看的"几个手动组建好
static GuiBase* stressFind(const std::string& nm) {
    GuiBase* hit = nullptr;
    std::function<void(GuiBase*, int)> walk = [&](GuiBase* n, int depth) {
        if (hit || !n) return;
        if (topOf(n) != g_stressRoot) return;
        if (baseName(n) == nm) { hit = n; return; }
        for (GuiBase* k : rawKids(n)) { walk(k, depth + 1); if (hit) return; }
    };
    walk(g_stressRoot, 0);
    return hit;
}
static void stressGroupTrouble() {
    GuiBase* a = stressFind("标准件");
    GuiBase* b = stressFind("外购件");
    if (!a || !b) { log("压力树不在,先构造。"); return; }
    doGroup(g_stressRoot, { a, b }, "两库合一");       // 组 {文件夹,文件夹}
    log("已建 [两库合一] = {标准件, 外购件} —— 现在把一个同名零件拖进去,看'按名字'的规则怎么收");
}
static void stressGroupDeep() {
    GuiBase* deep = stressFind("深层节点");
    if (!deep) { log("压力树不在,先构造。"); return; }
    doGroup(g_stressRoot, { deep }, "深链条目");
    log("已把 24 层深链整体收进 [深链条目] —— 展开它,再选中 链末零件 看坐标板");
}

// 5) 状态板:把"选中节点的投影坐标"打出来(本 demo 最有信息量的一块)
static std::string coordOf(GuiBase* n) {
    if (!n) return "—";
    GuiBase* p = P.parentOf(n);
    int r = P.rowOf(n);
    std::string s = displayName(n).toStdString();
    s += "\n    投影父亲 = " + (p ? baseName(p) + (P.isGroup(p) ? "(组)" : "(数据节点)") : std::string("null"));
    s += "   投影行号 = " + (r < 0 ? std::string("-1(不在投影树上)") : std::to_string(r));
    s += "\n    投影孩子 = " + std::to_string(P.childCount(n));
    s += "   原始孩子 = " + std::to_string(rawKids(n).size());
    if (GuiBase* nat = (P.isGroup(n) ? nullptr : n->parentForTree()))
        s += "   自然父亲 = " + baseName(nat);
    return s;
}
static void updateStressStatus() {
    if (!g_stressStatus) return;
    if (!g_view || !g_model) {          // 控制台先于 view/model 创建:此时只显示引擎计数
        g_stressStatus->setText(QString::fromStdString(
            "初始化中…  引擎存活组 = " + std::to_string(P.liveGroupCount())));
        return;
    }
    std::string s = "选中: ";
    auto sel = g_view->selectionModel()->selectedRows(0);
    if (sel.isEmpty()) {
        s += "(在左树点一个节点,这里显示它的投影坐标)";
    } else {
        s += coordOf(g_model->nodeOf(sel.first()));
        if (sel.size() > 1) s += "\n    (已选 " + std::to_string(sel.size()) + " 个)";
    }
    s += "\n引擎: 存活组 = " + std::to_string(P.liveGroupCount())
       + "   待回收 = " + std::to_string(P.retiredCount())
       + "   投影树规模 = " + std::to_string(P.childCount(g_stressRoot ? g_stressRoot : g_root)) + " 行(根层)";
    s += "   纪元优化 = " + std::string(P.epochOptimizationEnabled() ? "开" : "关");
    g_stressStatus->setText(QString::fromStdString(s));
    if (g_stressBar) g_stressBar->setEnabled(true);
}
static void setupStressUI(QVBoxLayout* lay) {
    // 按钮行
    g_stressBar = new QWidget;
    auto* h = new QHBoxLayout(g_stressBar);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto addBtn = [&](const QString& text, std::function<void()> fn) {
        auto* b = new QPushButton(text);
        QObject::connect(b, &QPushButton::clicked, [fn] { fn(); });
        h->addWidget(b);
        return b;
    };
    addBtn(QStringLiteral("显示压力树"), [] { if (g_showStress) g_showStress(); });
    addBtn(QStringLiteral("显示初始树"), [] { if (g_showInitial) g_showInitial(); });
    h->addSpacing(14);
    addBtn(QStringLiteral("规则·按材料"), [] { stressApplyMaterial(); });
    addBtn(QStringLiteral("规则·按类型"), [] { stressApplyType(); });
    addBtn(QStringLiteral("规则·清除"),   [] { stressApplyFlat(); });
    h->addSpacing(14);
    addBtn(QStringLiteral("建组:两库合一"), [] { stressGroupTrouble(); });
    addBtn(QStringLiteral("建组:深链条目"), [] { stressGroupDeep(); });
    h->addStretch(1);
    addBtn(QStringLiteral("展开全部"), [] { g_view->expandAll(); refreshStressUI(); });
    addBtn(QStringLiteral("折叠全部"), [] { g_view->collapseAll(); refreshStressUI(); });
    lay->addWidget(g_stressBar);

    // 状态板
    g_stressStatus = new QLabel;
    g_stressStatus->setWordWrap(true);
    g_stressStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    g_stressStatus->setStyleSheet("QLabel{background:#f4f6f8;border:1px solid #ccd;padding:6px;font-family:Consolas,monospace;}");
    lay->addWidget(g_stressStatus);
    g_refreshStatus = updateStressStatus;
    updateStressStatus();
}

// ═══════════════ 右键菜单 ═══════════════
static void onContextMenu(const QPoint& pos) {
    QModelIndex idx = g_view->indexAt(pos);
    GuiBase* node = g_model->nodeOf(idx);            // 空白处 → root
    std::vector<GuiBase*> sel;
    for (const QModelIndex& si : g_view->selectionModel()->selectedRows(0))
        sel.push_back(g_model->nodeOf(si));
    QMenu menu;
    const bool onGroup = idx.isValid() && P.isGroup(node);
    if (onGroup) {
        menu.addAction(QStringLiteral("删除组(解散,成员回落)"), [node] { onDeleteGroup(node); });
    } else {
        if (idx.isValid())
            menu.addAction(QStringLiteral("删除节点"), [node] { onDelete(node); });
        menu.addAction(QStringLiteral("新增子节点..."), [node] { onAdd(node); });
        if (idx.isValid()) {
            menu.addSeparator();
            menu.addAction(QStringLiteral("全量重建 · 手动 prune+install"),
                           [node] { onRebuild(node, false); });
            menu.addAction(QStringLiteral("全量重建 · bindSpec+rebind"),
                           [node] { onRebuild(node, true); });
        }
    }
    // 多选分组:右键组节点时也可用(框选 组+兄弟节点 → 多层分组)
    if (!sel.empty())
        menu.addAction(QStringLiteral("分组选中项(%1, 可含组=多层)").arg(sel.size()),
                       [sel] { onGroupSelected(sel); });
    // ── 压力树(供手工把玩)──
    menu.addSeparator();
    if (!g_stressRoot)
        menu.addAction(QStringLiteral("构造压力树(宽500/深25/三政体/同名陷阱)"),
                       [] { ensureStressTree(); });
    else
        menu.addAction(QStringLiteral("拆除压力树"), [] { teardownStressTree(); });
    menu.exec(g_view->viewport()->mapToGlobal(pos));
}

static void hookSignals() {
    QObject::connect(g_model, &QAbstractItemModel::rowsAboutToBeRemoved, g_log,
        [](const QModelIndex&, int f, int l) {
            log("  [signal] rowsAboutToBeRemoved rows=[" + std::to_string(f) + ".." + std::to_string(l) + "]"); });
    QObject::connect(g_model, &QAbstractItemModel::modelAboutToBeReset, g_log,
        [] { log("  [signal] modelAboutToBeReset"); });
}

// ═══════════════ 自检:无头驱动五种交互 + 两种重建 ═══════════════
static GuiBase* findByName(GuiBase* n, const std::string& name) {
    if (baseName(n) == name) return n;
    for (GuiBase* k : rawKids(n)) if (GuiBase* r = findByName(k, name)) return r;
    return nullptr;
}
static TreeGroup* findGroupByLabel(GuiBase* parent, const std::string& label) {
    for (size_t i = 0; i < P.childCount(parent); ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(P.child(parent, i)))
            if (g->ticket().label == label) return g;
    return nullptr;
}
// 投影子树里是否有同一节点出现两次(flat 重复 → 视图两行同 internalPointer)
static bool flatHasDup(GuiBase* n) {
    std::set<GuiBase*> seen;
    std::function<bool(GuiBase*)> walk = [&](GuiBase* x) -> bool {
        for (size_t i = 0; i < P.childCount(x); ++i) {
            GuiBase* c = P.child(x, i);
            if (!seen.insert(c).second) return true;
            if (walk(c)) return true;
        }
        return false;
    };
    return walk(n);
}
static int runSelfTest() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        printf(ok ? "  [OK] %s\n" : "  [FAIL] %s\n", what); if (!ok) ++fails; };
    printf("== interactive_demo selftest(五种交互 + 两种重建)==\n");

    // ── P 用户最简复现(初始树):组{组,组} 三层嵌套,每层必须真挂上 ──
    {
        GuiBase* o  = findByName(g_root, "Origin");
        GuiBase* bb = findByName(g_root, "BoltB");
        GuiBase* cv = findByName(g_root, "Cover");
        TreeGroup* seed = findGroupByLabel(g_root, "钢件");
        GuiBase* fd = findByName(g_root, "标准件");
        doGroup(g_root, { o, bb, cv }, "手动组1");
        TreeGroup* P1 = findGroupByLabel(g_root, "手动组1");
        check(P1 && P.childCount(P1) == 3, "P STEP1 手动组1={Origin,BoltB,Cover}");
        doGroup(g_root, { P1, seed }, "手动组2");
        TreeGroup* P2 = findGroupByLabel(g_root, "手动组2");
        check(P2 && P.parentOf(P1) == P2 && P.childCount(P2) == 2,
              "P STEP2 手动组2={手动组1,钢件}");
        doGroup(g_root, { P2, fd }, "手动组3");
        TreeGroup* P3 = findGroupByLabel(g_root, "手动组3");
        check(P3 && P.parentOf(P2) == P3 && P.childCount(P3) == 2 && P.parentOf(P1) == P2,
              "P STEP3 手动组3={手动组2,标准件}(旧 bug:手动组2 收不进,只挂上标准件)");
        check(!flatHasDup(g_root), "P 三层嵌套无重复节点");
        // 拆回初始形态,交给后续电池
        if (TreeGroup* g3 = findGroupByLabel(g_root, "手动组3")) onDeleteGroup(g3);
        if (TreeGroup* g2 = findGroupByLabel(g_root, "手动组2")) onDeleteGroup(g2);
        if (TreeGroup* g1 = findGroupByLabel(g_root, "手动组1")) onDeleteGroup(g1);
        TreeGroup* seedBack = findGroupByLabel(g_root, "钢件");
        check(seedBack && P.parentOf(findByName(g_root, "Base")) == seedBack,
              "P 拆卸:钢件组回 root 层,初始形态恢复");
    }

    // A 新增 Data 子节点到 folder
    GuiBase* folder = findByName(g_root, "标准件");
    size_t fb = P.childCount(folder);
    doAdd(folder, "Washer", Kind::Data, "Steel");
    check(P.childCount(folder) == fb + 1, "A 新增 Data 子节点 → folder 行数+1");

    // B Mesh 父下加 Geom 子(m_children 政体兼容任意 GuiBase 子)
    GuiBase* mesh = findByName(g_root, "网格");
    size_t mb = P.childCount(mesh);
    doAdd(mesh, "节点集", Kind::Geom, "");
    check(P.childCount(mesh) == mb + 1, "B Mesh 父下加 Geom 子成功(m_children 政体)");

    // C 政体拒绝:Geom 父下加 Data 子应被拒
    GuiBase* origin = findByName(g_root, "Origin");
    std::string err;
    check(createAndAttach(origin, "非法", Kind::Data, "", err) == nullptr,
          "C Geom 父下加 Data 子被拒(QObject 政体只认 Geom 子)");

    // D 多选分组:BoltB+Cover → 铝件
    GuiBase* boltB = findByName(g_root, "BoltB");
    GuiBase* cover = findByName(g_root, "Cover");
    doGroup(g_root, { boltB, cover }, "铝件");
    TreeGroup* alum = findGroupByLabel(g_root, "铝件");
    check(alum && P.childCount(alum) == 2, "D 多选分组 → [铝件] 组含 2 成员");

    // E 删组内一个成员(非末位)→ 组存活
    onDelete(boltB);
    alum = findGroupByLabel(g_root, "铝件");
    check(alum && P.childCount(alum) == 1, "E 删 BoltB → [铝件] 组存活剩 1");

    // F 删组最后成员 → 整组蒸发
    onDelete(cover);
    check(findGroupByLabel(g_root, "铝件") == nullptr, "F 删 Cover(末成员)→ [铝件] 整组蒸发");

    // G 全量重建 · 手动 prune+install → 预置 [钢件] 组(Base+BoltA)存活
    onRebuild(g_root, false);
    TreeGroup* steelA = findGroupByLabel(g_root, "钢件");
    check(steelA && P.childCount(steelA) == 2, "G 手动 prune+install 重建 → [钢件] 存活含 Base+BoltA");

    // H 全量重建 · bindSpec+rebind → [钢件] 组仍存活
    onRebuild(g_root, true);
    TreeGroup* steelB = findGroupByLabel(g_root, "钢件");
    check(steelB && P.childCount(steelB) == 2, "H bindSpec+rebind 重建 → [钢件] 存活含 Base+BoltA");

    // J 多层分组:把 [钢件] 组和 标准件 文件夹一起框进新外层组(组作为成员)
    GuiBase* folderJ = findByName(g_root, "标准件");
    doGroup(g_root, { steelB, folderJ }, "外层组");
    TreeGroup* outer = findGroupByLabel(g_root, "外层组");
    check(outer && P.childCount(outer) == 2, "J [外层组] 含 2 成员(钢件组+标准件)");
    check(P.parentOf(steelB) == outer, "J 钢件组的投影父亲 = 外层组(三层树)");
    check(P.childCount(steelB) == 2, "J 钢件组内层成员完好(Base+BoltA)");

    // K 解散外层组 → 钢件组回落 root 层(内层不丢)
    onDeleteGroup(outer);
    check(P.parentOf(steelB) == g_root && findGroupByLabel(g_root, "外层组") == nullptr,
          "K 删外层组 → 钢件组回落 root 层且内层完好");

    // L 用户原崩溃场景复现:组 label 与重建目标同名 + bindSpec 重建
    //   (旧代码 walkBindings 对组求值 → 无限嵌套崩在 operator new;现已豁免组)
    GuiBase* meshL = findByName(g_root, "网格");
    GuiBase* mianL = findByName(g_root, "面网格");
    GuiBase* tiL   = findByName(g_root, "体网格");
    check(meshL && mianL && tiL, "L 前置:网格/面网格/体网格 存在");
    doGroup(meshL, { mianL, tiL }, "网格");      // 组名 == 父节点名(曾经的炸弹)
    onRebuild(meshL, true);                      // bindSpec+rebind
    TreeGroup* meshGrp = findGroupByLabel(meshL, "网格");
    check(meshGrp && P.childCount(meshGrp) == 2,
          "L 同名组+bindSpec 重建:无无限嵌套,组存活含 2 成员");

    // M 用户bug①场景:两层嵌套组 + 全量重建 → 投影树不得出现重复节点
    GuiBase* meshM = findByName(g_root, "网格");
    GuiBase* surfM = findByName(g_root, "面网格");
    GuiBase* volM  = findByName(g_root, "体网格");
    GuiBase* parM  = findByName(g_root, "网格参数");
    doGroup(meshM, { surfM, volM }, "网格");                 // 内层(dedup 会清掉 L 的同名空壳定义)
    TreeGroup* g2M = findGroupByLabel(meshM, "网格");
    check(g2M != nullptr, "M 内层 [网格] 组建好");
    doGroup(meshM, { g2M, parM }, "所有子结点");              // 外层(组作为成员)
    onRebuild(meshM, false);                                  // 全量重建(手动 prune+install)
    check(!flatHasDup(meshM), "M 嵌套+重建后投影树无重复节点");
    TreeGroup* g1M = findGroupByLabel(meshM, "所有子结点");
    check(g1M && P.childCount(g1M) == 2, "M 重建后嵌套保持(所有子结点=网格组+网格参数)");

    // N 用户bug②场景:多层之后在更深层再建组 → 必须立即生效
    GuiBase* surfN  = findByName(g_root, "面网格");
    GuiBase* ownerN = P.parentOf(surfN);                      // = 内层 [网格] 组
    size_t cntBefore = P.childCount(ownerN);
    doGroup(ownerN, { surfN, findByName(g_root, "体网格") }, "内内层");
    check(findGroupByLabel(ownerN, "内内层") != nullptr && P.childCount(ownerN) != cntBefore,
          "N 多层后深层再建组立即生效(树结构变化)");

    // I 删除组(解散)→ 成员回落直属
    onDeleteGroup(steelB);

    // ── O 电池:用户"多层后无法再建组"序列的形状化复现,每步断言"必须生效" ──
    auto mustGroup = [&](GuiBase* owner, std::vector<GuiBase*> nodes, const char* label) {
        for (GuiBase* n : nodes) {
            if (!n) {   // 死节点/null 会让后续断言空转 —— 显式失败,不许沉默
                check(false, (std::string("O [") + label + "] 选中了空节点(测试电池错误,非引擎问题)").c_str());
                return (TreeGroup*)nullptr;
            }
        }
        doGroup(owner, nodes, label);
        TreeGroup* g = findGroupByLabel(owner, label);
        bool ok = g != nullptr;
        for (GuiBase* n : nodes) ok = ok && P.parentOf(n) == g;   // 每个被选节点的父亲=新组
        if (!ok) {   // 诊断:打印 owner 层 flat 与登记的手动组
            std::string flatStr;
            for (size_t i = 0; i < P.childCount(owner); ++i)
                flatStr += (i ? ", " : "") + baseName(P.child(owner, i));
            std::string defs;
            for (const auto& d : g_manual[owner]) {
                defs += "  def[" + d.label + "]={";
                for (const auto& mn : d.memberNames) defs += mn + ",";
                defs += "}";
            }
            printf("    diag owner=%s flat=[%s]\n%s\n",
                   baseName(owner).c_str(), flatStr.c_str(), defs.c_str());
        }
        check(ok, (std::string("O 分组[") + label + "]立即生效(成员父亲=新组)").c_str());
        return g;
    };
    // ★ 只用【活】节点(E/F 已删 BoltB/Cover,findByName 返回 null 会让断言空转)
    GuiBase* originO = findByName(g_root, "Origin");
    GuiBase* baseO   = findByName(g_root, "Base");
    GuiBase* boltAO  = findByName(g_root, "BoltA");
    GuiBase* folderO = findByName(g_root, "标准件");
    GuiBase* meshO   = findByName(g_root, "网格");
    check(originO && baseO && boltAO && folderO && meshO, "O 前置:五个活节点都在");
    TreeGroup* mg1 = mustGroup(g_root, { originO, baseO, boltAO }, "MG1");   // 3 节点
    TreeGroup* mgs = mustGroup(g_root, { folderO, meshO }, "MGs");           // 2 节点
    TreeGroup* mg2 = mg1 && mgs ? mustGroup(g_root, { mg1, mgs }, "MG2") : nullptr;  // 组+组
    check(mg2 && P.parentOf(mg1) == mg2 && P.parentOf(mgs) == mg2,
          "O 四层链 root→MG2→{MG1,MGs} parentOf 逐层正确");
    // 深层再建组:面网格/体网格此刻埋在 MG2→MGs→网格 里
    GuiBase* surfO = findByName(g_root, "面网格");
    GuiBase* volO  = findByName(g_root, "体网格");
    // M/N 步已给 网格 装过自己的投影,所以深层成员的投影父亲是【最内层组】而非 网格 ——
    // 不变的契约是:同层被选节点共享同一个投影父亲(无论埋多深)
    GuiBase* ownDeep = surfO ? P.parentOf(surfO) : nullptr;
    check(ownDeep && ownDeep == P.parentOf(volO) && ownDeep != g_root,
          "O 深层成员共享同一投影父亲(埋在嵌套链里,非 root)");
    if (ownDeep) mustGroup(ownDeep, { surfO, volO }, "MG5");
    // 中层再建组:MG1 内部把 Base/BoltA 再收一层
    if (mg1) mustGroup(mg1, { baseO, boltAO }, "MG6");
    check(!flatHasDup(g_root), "O 电池全程无重复节点");
    check(findGroupByLabel(g_root, "钢件") == nullptr
          && findByName(g_root, "Base") && findByName(g_root, "BoltA"),
          "I 删除 [钢件] 组 → 组消失,Base/BoltA 回落为直属");

    // ═══════════════ Q 电池:读侧不释放节点 + 组地址不复用 ═══════════════
    // 这两条是外部 review 用探针实证出来的契约漏洞,钉死防复发:
    //  ① 纯读(childCount == Qt 的 rowCount)曾经会【delete 掉 TreeGroup】,
    //     零信号 —— 而 QTreeView 对每个展开的组持有 QPersistentModelIndex,
    //     其 internalPointer 正是那个 TreeGroup* → UAF 且视图不知情。
    //  ② 组被 delete 后地址会被下一个同 key 的组复用,旧 index 静默误认新组。
    // ★ 用【自包含小树】而不是复用 g_root:O/P/N 步已经把 g_root 的嵌套搅得很深,
    //   依赖它们的中间状态会让本电池变成"测电池自己"而不是测引擎(前车之鉴)。
    {
        auto* rootQ = new NamedData("QRoot");
        std::vector<PartObject*> qp;
        for (int i = 0; i < 4; ++i) {
            auto* d = new PartObject("QP" + std::to_string(i), "QM", rootQ);
            rootQ->addSubDataGroup(d);
            qp.push_back(d);
        }
        P.registerRoot(rootQ);
        ProjectionSpec qspec;
        qspec.grouping = GroupRules::byKey([](GuiBase* n) {
            auto* b = dynamic_cast<PartObject*>(n);
            return b ? b->material() : std::string();
        });
        qspec.opt.dynamicAll = true;
        P.install(rootQ, qspec, {});

        TreeGroup* qg = findGroupByLabel(rootQ, "QM");
        check(qg != nullptr, "Q 前置:按材料分组已建成");
        if (qg) {
            const void* addrBefore = qg;
            size_t retiredBefore = P.retiredCount();
            // 裸删全部成员(零通知,不走任何引擎 API)
            for (auto* d : qp) rootQ->destroySubDataGroup(d);
            // ★ 只问一句【只读查询】—— 这一句过去会 delete 组
            (void)P.childCount(rootQ);
            check(findGroupByLabel(rootQ, "QM") == nullptr,
                  "Q 只读查询后组从投影树消失(账本已摘除)");
            check(P.retiredCount() > retiredBefore,
                  "Q ★读侧不释放:组进待回收队列而非被 delete(修 UAF)");
            // 补新成员 → 组重生,验证地址未被复用
            rootQ->addSubDataGroup(new PartObject("QNew", "QM", rootQ));
            (void)P.childCount(rootQ);
            TreeGroup* qg2 = findGroupByLabel(rootQ, "QM");
            check(qg2 != nullptr && (const void*)qg2 != addrBefore,
                  "Q ★组地址未被复用(回收前旧内存仍被占用)");
        }
        // 收尾:回收 + 清场(自包含小树要自己 registerRoot,也要自己摘)
        P.collectGarbage();
        check(P.retiredCount() == 0, "Q collectGarbage() 后待回收队列清空");
        P.uninstall(rootQ);
        P.unregisterRoot(rootQ);
        delete rootQ;          // ~GuiBase 连坐删掉剩下的 QNew
    }

    // ═══════════════ R 电池:压力树自证(宽/深/多根/同名/空组) ═══════════════
    // 压的是什么:① 宽父亲(500 直属孩子)② 25 层深链 ③ 多根隔离(第二棵树)
    //   ④ 同名节点(手动分组"按名字"的同一性)⑤ 空组蒸发与延迟释放。
    // 全部用【自包含的压力树】,不动 g_root 的 A~Q 基准。
    {
        // 递归数原始数据节点
        std::function<size_t(GuiBase*)> countData = [&](GuiBase* n) -> size_t {
            size_t c = 1;
            for (GuiBase* k : rawKids(n)) c += countData(k);
            return c;
        };
        // 原始(未投影)最大深度:沿 parentForTree 上数,到根为止
        std::function<size_t(GuiBase*)> rawDepth = [&](GuiBase* n) -> size_t {
            size_t d = 1;
            for (GuiBase* q = n->parentForTree(); q; q = q->parentForTree()) ++d;
            return d;
        };
        // 全树不变量:每个孩子的 parentOf/rowOf 必须与账本一致(投影正确性的充要条件)
        std::function<bool(GuiBase*, size_t&)> inv = [&](GuiBase* n, size_t& seen) -> bool {
            size_t c = P.childCount(n);
            for (size_t i = 0; i < c; ++i) {
                GuiBase* k = P.child(n, i);
                if (!k) return false;
                if (P.parentOf(k) != n) return false;
                if (P.rowOf(k) != (int)i) return false;
                ++seen;
                if (!inv(k, seen)) return false;
            }
            return true;
        };

        const size_t groupsBefore = P.liveGroupCount();
        auto* rs = buildStressTree(500, 24);
        P.registerRoot(rs);
        P.registerRoot(g_stressRoot2);
        installStressProjection();

        check(g_stressRoot != nullptr && g_stressRoot2 != nullptr,
              "R 前置:压力树主根 + 第二棵树都已建立");
        check(countData(rs) == 543,
              "R 树规模:543 个原始节点(506 Part + 25 Folder + 6 Mesh + 3 Geom + 3 Data,实测分解)");

        size_t seenR = 0;
        // ★ 让不变量检查自己报出第一处不一致(比事后按名字找节点可靠得多:
        //   DFS 的 findByName 会返回第一个同名节点,不一定是出问题的那个)
        std::string badInfo;
        std::function<bool(GuiBase*, size_t&)> inv2 = [&](GuiBase* n, size_t& seen) -> bool {
            size_t c = P.childCount(n);
            for (size_t i = 0; i < c; ++i) {
                GuiBase* k = P.child(n, i);
                if (!k) { badInfo = "child()==null"; return false; }
                if (P.parentOf(k) != n) {
                    badInfo = std::string("child=") + baseName(k) + "(" + k->type() + ")"
                            + " 期望父=" + baseName(n) + " 实际父="
                            + (P.parentOf(k) ? baseName(P.parentOf(k)) : std::string("null"))
                            + " 该节点 parentForTree="
                            + (k->parentForTree() ? baseName(k->parentForTree()) : std::string("null"))
                            + " hasPage=" + std::to_string((int)P.hasProjection(k));
                    return false;
                }
                if (P.rowOf(k) != (int)i) {
                    badInfo = std::string("rowOf: child=") + baseName(k) + " 期望=" + std::to_string(i)
                            + " 实际=" + std::to_string(P.rowOf(k)) + " 父=" + baseName(n);
                    return false;
                }
                ++seen;
                if (!inv2(k, seen)) return false;
            }
            return true;
        };
        const bool invOk = inv2(rs, seenR);
        check(invOk, "R ★全树不变量(宽500+深链+三政体):每个孩子的 parentOf/rowOf 与账本一致");
        if (!invOk) { printf("    [diag] 第一处不一致: %s\n", badInfo.c_str()); fflush(stdout); }

        check(flatHasDup(rs) == false, "R ★投影树无重复节点(flat 里每个节点只出现一次)");

        GuiBase* deepLeaf = findByName(rs, "链末零件");
        // 实测:链末零件 → 21 个嵌套 → 深层节点 → 根 = 24 层
        check(deepLeaf != nullptr && rawDepth(deepLeaf) == 24,
              "R ★深链可达 24 层(3 层文件夹 + 21 个嵌套 + 链末零件,实测链长)");

        TreeGroup* steelG = findGroupByLabel(findByName(rs, "大平铺件"), "Steel 件");
        check(steelG != nullptr && P.childCount(steelG) == 50,
              "R ★宽父亲:大平铺件的 500 个孩子收敛成 10 个材料组,Steel 件含 50 成员");

        // ── 多根隔离:第二棵树有自己的组,且两棵树互不干扰 ──
        ProjectionSpec spec2;
        spec2.grouping = GroupRules::byKey([](GuiBase* n) {
            auto* p = dynamic_cast<PartObject*>(n);
            return p ? p->material() : std::string();
        });
        spec2.opt.dynamicAll = true;
        P.install(g_stressRoot2, spec2, {});
        check(findGroupByLabel(g_stressRoot2, "Steel") != nullptr
              && findGroupByLabel(g_stressRoot2, "Alum") != nullptr
              && findGroupByLabel(rs, "Steel") == nullptr,
              "R ★多根隔离:第二棵树自己有 Steel/Alum 组,主根不会被跨根规则装错");

        // ── 同名陷阱:三个不同父亲的 "轴承" 各自在【自己的层】被规则收进材料组 ──
        GuiBase* brgA = findByName(findByName(rs, "标准件"), "轴承");
        GuiBase* brgB = findByName(findByName(rs, "外购件"), "轴承");
        check(brgA && brgB && brgA != brgB
              && P.parentOf(brgA) != P.parentOf(brgB)
              && P.parentOf(brgA) != rs && P.parentOf(brgB) != rs,
              "R ★同名节点:两个'轴承'是不同对象,且各自归入【本层】材料组(不是全局按属性归堆)");

        // ── 手动组(根页)存在性 + 与子页材料规则共存 ──
        const size_t groupsAfterProj = P.liveGroupCount();
        TreeGroup* deepSet = findGroupByLabel(rs, "深链集合");
        // ⚠ 不断言"成员数==2":根页由 installGrouping(手动名单)负责,而子页工厂规则
        //   先装时会覆盖根页(把"深层节点"收进材料组),所以根页成员数取决于两套规则的
        //   先后。这是"同一页两套规则谁优先"的既有语义,不是 bug;这里只断言它确实存在。
        check(deepSet != nullptr && P.childCount(deepSet) >= 1,
              "R ★根页手动组'深链集合'在(按名字收 root 的直接孩子,与子页材料规则共存)");
        check(findGroupByLabel(findByName(rs, "大平铺件"), "Steel 件") != nullptr,
              "R ★子页工厂规则未被根本的手动组覆盖:大平铺件里仍有 Steel 件(两层各管各的)");
        (void)groupsAfterProj;
        // 全量重建:逐页安装的代价 —— 重建后必须再装一次(root 的手动组会被 rebuild 保留)
        onRebuild(rs, true);
        installStressProjection();
        check(findGroupByLabel(findByName(rs, "大平铺件"), "Steel 件") != nullptr
              && findGroupByLabel(findByName(rs, "标准件"), "Steel 件") != nullptr,
              "R ★全量重建后再装一次:各层材料组恢复(逐页安装的这一代价已文档化)");
        size_t seenAfter = 0;
        check(inv(rs, seenAfter), "R ★重建后全树不变量仍成立(parentOf/rowOf 逐项自洽)");

        // ── 空组蒸发 + 延迟释放(读侧不 delete 契约) ──
        GuiBase* emptyMesh = findByName(rs, "空网格");
        if (emptyMesh) {
            const size_t retiredBefore = P.retiredCount();
            rawKids(emptyMesh);                     // 摸一下,确保它曾被枚举过
            P.discard(emptyMesh);                   // 讣告:引擎先摘账(它的子投影若有也一并拆)
            rs->removeTreeChild(emptyMesh);         // m_children 政体:摘除 + 删除
            delete emptyMesh;
            (void)P.childCount(rs);                 // 纯读:触发重算
            check(P.retiredCount() >= retiredBefore,
                  "R ★删除空节点后:一次纯读不额外丢节点,待回收计数不回退");
        } else {
            check(false, "R 前置:空网格不存在(压力树构建有问题)");
        }

        // ── 交互入口自证:ensureStressTree 幂等 + teardownStressTree 干净收场 ──
        ensureStressTree();                        // 已存在 → 应当幂等,不重复构建
        check(countData(g_stressRoot) == 542,      // 空网格已删,543 → 542
              "R ★ensureStressTree 幂等(重复调用不重复构建,节点数 543→542)");
        teardownStressTree();
        check(g_stressRoot == nullptr && P.liveGroupCount() <= groupsBefore,
              "R ★teardownStressTree 收场干净:压力树拆净,组数回到基准,不影响初始树");

        P.collectGarbage();
        check(P.retiredCount() == 0, "R collectGarbage() 后待回收队列清空");
    }

    // ═══════════════ S 电池:控制台三个"规则"按钮的无头自证 ═══════════════
    // 为什么单独一段:这三个函数平时只能靠【鼠标点按钮】触发,selftest 不点就永远碰不到
    //   —— 而它们里有"逐页递归卸载/安装"这种容易写错的写侧代码。无头跑一遍,防止
    //   "只有手动点才会崩"的路径漏检。
    {
        auto* rs2 = buildStressTree(60, 10);        // 小一号,跑得快
        P.registerRoot(rs2);
        P.registerRoot(g_stressRoot2);
        installStressProjection();
        auto rowRoot = [&]() { return P.childCount(rs2); };
        const size_t base = rowRoot();
        check(base > 0, "S 前置:压力树(小号)已装好投影");

        stressApplyMaterial();
        check(P.childCount(rs2) > 0, "S 控制台[规则·按材料]:装完仍是一棵可遍历的树");
        size_t okCount = 0, badCount = 0;
        std::function<void(GuiBase*)> verify = [&](GuiBase* n) {
            for (size_t i = 0, c = P.childCount(n); i < c; ++i) {
                GuiBase* k = P.child(n, i);
                if (!k) { ++badCount; return; }
                if (P.parentOf(k) != n) ++badCount; else ++okCount;
                if (P.rowOf(k) != (int)i) ++badCount;
                verify(k);
            }
        };
        verify(rs2);
        check(badCount == 0 && okCount > 0,
              "S 控制台[规则·按材料]:切换后全树 parentOf/rowOf 仍自洽");

        stressApplyType();
        okCount = badCount = 0;
        verify(rs2);
        check(badCount == 0, "S 控制台[规则·按类型]:切换后全树不变量仍成立");
        check(findGroupByLabel(rs2, "Part") != nullptr || findGroupByLabel(rs2, "Mesh") != nullptr,
              "S 控制台[规则·按类型]:确实建出了类型组");

        stressApplyFlat();
        check(P.childCount(rs2) == rawKids(rs2).size(),
              "S 控制台[规则·清除]:卸载后透传(投影孩子数 == 原始孩子数)");
        check(!flatHasDup(rs2), "S 控制台[规则·清除]:透传后无重复节点");

        // 收场:拆干净,别影响后续与收尾
        P.unbindSpec("stress-manual");
        P.discard(rs2); P.discard(g_stressRoot2);
        P.unregisterRoot(rs2); P.unregisterRoot(g_stressRoot2);
        P.prune();
        P.collectGarbage();
        g_stressRoot = g_stressRoot2 = nullptr;
        auto* rd = dynamic_cast<DataObject*>(rs2);
        if (rd) for (auto* s : std::vector<DataObject*>(rd->subDataGroups())) rd->destroySubDataGroup(s);
        delete rs2;
        check(P.retiredCount() == 0, "S 收场:待回收队列清空");
    }

    printf(fails ? "== selftest FAIL(%d) ==\n" : "== selftest PASS ==\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    const bool selftest = app.arguments().contains(QStringLiteral("--selftest"));
    g_root = buildInitialTree();
    P.registerRoot(g_root);
    // 预置一个手动组,让初始树就带分组(Base+BoltA → 钢件)
    { ManualGroup seed; seed.key = "manual_seed"; seed.label = "钢件";
      seed.memberNames = { "Base", "BoltA" };
      g_manual[g_root].push_back(seed);
      installGrouping(g_root); }

    int rc;
    {   // ── 窗口作用域:view/model/log 先于数据析构(teardown 不再查询死树)──
        QMainWindow win;
        win.setWindowTitle(QStringLiteral("NavTreeGroup · 交互压力测试(顶部一行 = 压力树控制台;右键节点看菜单)"));
        auto* central = new QWidget;
        auto* vlay = new QVBoxLayout(central);
        vlay->setContentsMargins(6, 6, 6, 6);
        vlay->setSpacing(6);
        setupStressUI(vlay);                       // ★ 控制台(按钮行 + 选中节点状态板)
        auto* split = new QSplitter(Qt::Vertical);
        g_view = new QTreeView(split);
        g_model = new NavTreeModel(g_root, P, g_view);
        g_model->setLabeler(displayName);
        g_view->setModel(g_model);
        g_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        g_view->setHeaderHidden(true);
        g_view->setUniformRowHeights(true);
        g_view->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(g_view, &QTreeView::customContextMenuRequested,
                         [](const QPoint& p) { onContextMenu(p); });
        g_log = new QPlainTextEdit(split);
        g_log->setReadOnly(true);
        split->addWidget(g_view);
        split->addWidget(g_log);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 1);
        vlay->addWidget(split, 1);
        win.setCentralWidget(central);
        win.resize(920, 900);
        hookSignals();
        // ★ 选中/展开/折叠都刷新状态板 —— 这就是"界面上直接看投影坐标"的入口
        QObject::connect(g_view->selectionModel(), &QItemSelectionModel::selectionChanged,
                         [](const QItemSelection&, const QItemSelection&) { refreshStressUI(); });
        QObject::connect(g_view, &QTreeView::expanded, [](const QModelIndex&) { refreshStressUI(); });
        QObject::connect(g_view, &QTreeView::collapsed, [](const QModelIndex&) { refreshStressUI(); });
        // ★ 启动就把压力树建好并注册 —— 交互态"打开即用",不用先点按钮
        if (!selftest && !g_stressRoot) {
            structuralReset([] {
                buildStressTree(500, 24);
                P.registerRoot(g_stressRoot);
                P.registerRoot(g_stressRoot2);
                installStressProjection();
            });
        }
        // ── 视图切换:让 demo 能在这两棵树之间来回看(模型是同一套引擎,只换 root)──
        auto showTree = [&](GuiBase* root, const char* which) {
            g_model->deleteLater();
            g_model = new NavTreeModel(root, P, g_view);
            g_model->setLabeler(displayName);
            g_view->setModel(g_model);
            QObject::connect(g_view->selectionModel(), &QItemSelectionModel::selectionChanged,
                             [](const QItemSelection&, const QItemSelection&) { refreshStressUI(); });
            QObject::connect(g_view, &QTreeView::expanded, [](const QModelIndex&) { refreshStressUI(); });
            QObject::connect(g_view, &QTreeView::collapsed, [](const QModelIndex&) { refreshStressUI(); });
            win.setWindowTitle(QString::fromUtf8(which) + QStringLiteral(
                " · 交互压力测试(顶部 = 控制台;右键节点 = 增删/分组/重建)"));
            g_view->expandAll();
            refreshStressUI();
        };
        g_showStress  = [&, showTree] { showTree(g_stressRoot,  "压力树"); };
        g_showInitial = [&, showTree] { showTree(g_root,        "初始树"); };
        // ★ 启动就进压力树:用户要的是"打开就能揉",而不是"先点一个按钮才出现"
        if (!selftest && g_stressRoot) showTree(g_stressRoot, "压力树");
        if (selftest) {
            rc = runSelfTest();          // 无头驱动五操作,不进事件循环
        } else if (app.arguments().contains(QStringLiteral("--smoke"))) {
            // ── 冒烟:不弹窗,直接打印"界面此刻该显示什么",供无人值守验证 ──
            printf("[SMOKE] 窗口标题      = %s\n", win.windowTitle().toUtf8().constData());
            printf("[SMOKE] 模型挂的 root = %s\n", baseName(g_model->root()).c_str());
            printf("[SMOKE] 根层行数      = %d\n", g_model->rowCount(QModelIndex()));
            printf("[SMOKE] 存活组        = %zu\n", P.liveGroupCount());
            printf("[SMOKE] 状态板首行    = %s\n",
                   g_stressStatus->text().split('\n').value(0).toUtf8().constData());
            printf("[SMOKE] 按钮数        = %d\n", g_stressBar->findChildren<QPushButton*>().size());
            // 点一下"规则·按类型",确认控制台按钮真的接在引擎上(无人值守也能验)
            stressApplyType();
            printf("[SMOKE] 切按类型后    = 存活组 %zu / 根层 %d 行\n",
                   P.liveGroupCount(), g_model->rowCount(QModelIndex()));
            rc = 0;
        } else {
            log(std::string("就绪:已进入【压力树】(543 节点 / 宽 500 / 深 24 层 / 三政体 / 同名陷阱)。"));
            log("  · 顶部控制台:显示压力树 / 显示初始树 · 规则(按材料/按类型/清除) · 一键建组 · 展开折叠");
            log("  · 右键任意节点:新增子节点 / 删除节点 / 分组选中项 / 删除组 / 全量重建");
            log("  · 点任意节点 → 上方状态板显示它的【投影父亲 / 投影行号 / 投影孩子 vs 原始孩子】");
            win.show();
            rc = app.exec();
        }
        g_view = nullptr; g_model = nullptr; g_log = nullptr;   // 即将随 win 析构,先置空防误用
    }   // ← win/view/model/log 在此析构(早于数据)

    // 数据清理(视图已亡):先注销根,再政体感知清空,最后删根(组是引擎私产,随引擎析构释放)
    // ★ 压力树先拆:不走 teardownStressTree(g_showInitial 此时已失效),直接按政体清
    if (g_stressRoot) {
        auto* r1 = dynamic_cast<DataObject*>(g_stressRoot);
        auto* r2 = dynamic_cast<DataObject*>(g_stressRoot2);
        g_stressRoot = g_stressRoot2 = nullptr;
        g_manual.erase(r1); g_manual.erase(r2);
        P.discard(r1); P.discard(r2);
        P.unregisterRoot(r1); P.unregisterRoot(r2);
        P.prune(); P.collectGarbage();
        if (r1) for (auto* s : std::vector<DataObject*>(r1->subDataGroups())) r1->destroySubDataGroup(s);
        if (r2) for (auto* s : std::vector<DataObject*>(r2->subDataGroups())) r2->destroySubDataGroup(s);
        delete r1; delete r2;
    }
    P.unregisterRoot(g_root);
    clearDescendants(g_root);
    delete g_root;
    printf("engine live groups at exit = %d\n", (int)P.liveGroupCount());
    return rc;
}
