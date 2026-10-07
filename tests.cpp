// ═══════════════════════════════════════════════════════════════
//  unit_tests:把流程切成 16 块,每块打印【职责 → 输入 → 输出 → 校验】
// ═══════════════════════════════════════════════════════════════
//  T1 ~T6  流水线纯函数(projection_stages.h)—— 不碰引擎,裸输入裸输出
//  T7 ~T12 引擎行为:端到端投影/缓存/组复用/坐标/蒸发/多层
//  T13~T14 生命周期卫生:prune 剪僵尸 / bindSpec 跨重建重生
//  T15~T16 模型层:三要素自洽 / 事前坐标信号协议
//  运行:unit_tests.exe(仅 Qt Core,无窗口);退出码 = 失败块数
#ifndef NT_ENGINE_ONLY
#include <QAbstractItemModel>
#include <QAbstractItemModelTester>   // T18:Qt 官方模型协议测试器(QtTest 模块)
#endif
#include <cstdio>
#include <cstdint>
#include <chrono>        // T21:宽节点读侧性能下限
#include <string>
#include <vector>
#include <functional>
#ifdef Q_OS_WIN
#include <crtdbg.h>      // _CrtSetReportMode / _CrtSetReportFile
#include <windows.h>     // SetErrorMode
#endif
#include "guibase.h"
#include "treeprojection.h"
#include "projection_stages.h"
#ifndef NT_ENGINE_ONLY
#include "navtreemodel.h"          // 覆盖率构建(NT_ENGINE_ONLY)下不需要 Qt 模型层
#endif
#include "demonodes.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

static TreeProjection& P = TreeProjection::instance();
static int g_fail = 0;

// ═══════════════ 打印小件:让每块的输入输出可读 ═══════════════

static std::string nm(GuiBase* n) {
    if (!n) return "null";
    if (auto* g = dynamic_cast<TreeGroup*>(n)) return "[组]" + g->ticket().label;
    std::string s = demoNodeName(n);
    if (auto* p = dynamic_cast<PartObject*>(n)) {
        s += "(" + p->material();
        if (p->suppressed) s += ",压制";
        s += ")";
    }
    return s;
}
static std::string lst(const std::vector<GuiBase*>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + nm(v[i]);
    return s + "]";
}
static void hdr(const char* id, const char* name, const char* duty) {
    printf("\n══ %s %s ══\n  职责:%s\n", id, name, duty);
}
static void showIn(const std::string& s)  { printf("  输入:%s\n", s.c_str()); }
static void showOut(const std::string& s) { printf("  输出:%s\n", s.c_str()); }
static void check(bool ok, const std::string& what) {
    printf("  [%s] %s\n", ok ? "OK" : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
static void expectList(const std::vector<GuiBase*>& got,
                       const std::vector<GuiBase*>& want, const char* what) {
    bool ok = got.size() == want.size();
    for (size_t i = 0; ok && i < got.size(); ++i) ok = got[i] == want[i];
    printf("  预期:%s\n", lst(want).c_str());
    check(ok, what);
}

// ═══════════════ 数据小件 ═══════════════

struct World {                       // 一棵最小演示树(root 政体 = DataObject 自管)
    NamedData* root = nullptr;
    PartObject *base = nullptr, *boltA = nullptr, *boltB = nullptr, *cover = nullptr;
    SketchObject* sketch = nullptr;
};
static World makeWorld(bool suppressCover) {
    World w;
    w.root  = new NamedData("Root");
    w.base  = new PartObject("Base",  "Steel", w.root);
    w.boltA = new PartObject("BoltA", "Steel", w.root);
    w.boltB = new PartObject("BoltB", "Alum",  w.root);
    w.cover = new PartObject("Cover", "Alum",  w.root);
    w.cover->suppressed = suppressCover;
    w.sketch = new SketchObject("Sketch1", w.root);
    w.root->addSubDataGroup(w.base);
    w.root->addSubDataGroup(w.boltA);
    w.root->addSubDataGroup(w.boltB);
    w.root->addSubDataGroup(w.cover);
    w.root->addSubDataGroup(w.sketch);
    return w;
}
static void clearData(DataObject* d) {
    for (auto* s : std::vector<DataObject*>(d->subDataGroups())) {
        clearData(s);
        d->destroySubDataGroup(s);   // 摘除 + delete
    }
}
static void freeWorld(World& w) { clearData(w.root); delete w.root; w = {}; }

static std::function<bool(GuiBase*)> notSuppressed() {
    return [](GuiBase* n) { auto* p = dynamic_cast<PartObject*>(n); return !(p && p->suppressed); };
}
static GroupRule byMaterial() {
    return [](GuiBase* n) -> std::optional<GroupTicket> {
        if (auto* p = dynamic_cast<PartObject*>(n)) {
            GroupTicket t; t.key = p->material(); t.label = p->material() + " parts";
            return t;
        }
        return std::nullopt;         // 逃生口:非零件不进桶
    };
}
static TreeGroup* findGroup(GuiBase* parent, const std::string& key) {
    for (size_t i = 0, n = P.childCount(parent); i < n; ++i)
        if (auto* g = dynamic_cast<TreeGroup*>(P.child(parent, i)))
            if (g->ticket().key == key) return g;
    return nullptr;
}
static std::vector<GuiBase*> kids(GuiBase* n) {
    std::vector<GuiBase*> v;
    for (size_t i = 0, c = P.childCount(n); i < c; ++i) v.push_back(P.child(n, i));
    return v;
}
static GuiBase* topOf(GuiBase* n) {
    GuiBase* p = n;
    while (p) { GuiBase* q = p->parentForTree(); if (!q) break; p = q; }
    return p;
}

// 标准 Spec:过滤压制件 + 材料分组 + demoLess(与 gui_demo 同款)
static ProjectionSpec stdSpec() {
    return ProjectionSpec{ notSuppressed(), byMaterial(), demoLess, { false, true } };
}

// ═══════════════ T1~T6:流水线纯函数 ═══════════════

static void t1_filter() {
    hdr("T1", "ProjectionStages::filterLive", "Stage A 过滤 —— filter 为 false 的节点移出投影");
    World w = makeWorld(true);
    std::vector<GuiBase*> live = { w.base, w.boltA, w.boltB, w.cover, w.sketch };
    auto vis = ProjectionStages::filterLive(live, notSuppressed());
    showIn("live = " + lst(live) + ",  filter = 非压制件");
    showOut("vis  = " + lst(vis));
    expectList(vis, { w.base, w.boltA, w.boltB, w.sketch }, "Cover(压制)被滤掉,其余保序");
    auto all = ProjectionStages::filterLive(live, nullptr);
    check(all.size() == live.size(), "filter 为空 = 全员通过");
    freeWorld(w);
}

static void t2_bucket() {
    hdr("T2", "ProjectionStages::bucketByKey", "Stage B-1 分桶 —— 规则给票据,同 key 归一堆");
    World w = makeWorld(true);
    std::vector<GuiBase*> vis = { w.base, w.boltA, w.boltB, w.sketch };
    size_t participants = 0;
    auto buckets = ProjectionStages::bucketByKey(vis, nullptr /*dynamicAll*/, byMaterial(), participants);
    showIn("vis = " + lst(vis) + ",  grouping = 按材料(Sketch 返回 nullopt)");
    std::string out;
    for (auto& b : buckets) {
        if (!out.empty()) out += "; ";
        out += "桶{" + b.ticket.key + "}: " + lst(b.members);
    }
    showOut(out + "   participants=" + std::to_string(participants));
    check(buckets.size() == 2, "两个桶(Steel/Alum)");
    check(buckets[0].ticket.key == "Steel" && buckets[0].members.size() == 2,
          "Steel 桶 = [Base, BoltA](按出现顺序)");
    check(buckets[1].ticket.key == "Alum" && buckets[1].members.size() == 1,
          "Alum 桶 = [BoltB]");
    check(participants == 4, "Sketch 参与了计数但走了逃生口(nullopt → 不进桶,留直属)");
    freeWorld(w);
}

static void t3_collapse() {
    hdr("T3", "ProjectionStages::shouldCollapse", "Stage B-2 单组塌缩 —— 全员同组时不建废层级");
    World w = makeWorld(false);
    // 情形① 全员进同一个桶(比如把所有 DataObject 归入 "All")
    GroupRule all = [](GuiBase*) { GroupTicket t; t.key = "All"; t.label = "All"; return t; };
    std::vector<GuiBase*> vis = { w.base, w.boltA, w.boltB, w.cover, w.sketch };
    size_t part = 0;
    auto b1 = ProjectionStages::bucketByKey(vis, nullptr, all, part);
    bool c1 = ProjectionStages::shouldCollapse(b1, part, vis.size(), { true, true });
    showIn("vis 全员同 key, collapseIfSingleGroup=true");
    showOut(std::string("shouldCollapse = ") + (c1 ? "true" : "false"));
    check(c1, "全员一组 → 塌缩(组层级是废的)");
    // 情形② 有直属残余(Sketch 走逃生口)→ 不塌缩
    auto b2 = ProjectionStages::bucketByKey(vis, nullptr, byMaterial(), part);
    bool c2 = ProjectionStages::shouldCollapse(b2, part, vis.size(), { true, true });
    showIn("vis 分出 Steel/Alum 两桶 + Sketch 直属");
    showOut(std::string("shouldCollapse = ") + (c2 ? "true" : "false"));
    check(!c2, "多桶/有直属 → 不塌缩");
    freeWorld(w);
}

static void t4_reuse() {
    hdr("T4", "ProjectionStages::matchReusable", "Stage B-3 组复用匹配 —— 同 key 用旧组指针,落选的进淘汰名单");
    // 旧组:Steel/Alum;新桶:Steel/Copper → Steel 复用,Alum 淘汰,Copper 新建
    GroupTicket ts; ts.key = "Steel"; ts.label = "Steel parts";
    GroupTicket ta; ta.key = "Alum";  ta.label = "Alum parts";
    auto* gSteel = new TreeGroup(ts, nullptr);
    auto* gAlum  = new TreeGroup(ta, nullptr);
    std::vector<TreeGroup*> old = { gSteel, gAlum };
    std::vector<ProjectionStages::Bucket> buckets(2);
    buckets[0].ticket = ts;
    buckets[1].ticket.key = "Copper"; buckets[1].ticket.label = "Copper parts";
    std::vector<TreeGroup*> culled;
    auto reuse = ProjectionStages::matchReusable(buckets, old, culled);
    showIn("旧组 = [Steel@0x" + std::to_string((uintptr_t)gSteel) + ", Alum],  新桶 = [Steel, Copper]");
    showOut(std::string("reuse = [") + (reuse[0] == gSteel ? "Steel旧指针" : "?") + ", "
            + (reuse[1] ? "?" : "null(新建)") + "],  culled = ["
            + (culled.size() == 1 && culled[0] == gAlum ? "Alum" : "?") + "]");
    check(reuse[0] == gSteel, "Steel 桶复用旧组(指针稳定 → UI 状态存活)");
    check(reuse[1] == nullptr, "Copper 桶无旧组可复用 → 引擎将新建");
    check(culled.size() == 1 && culled[0] == gAlum, "Alum 未被复用 → 进淘汰名单(引擎级联拆+delete)");
    delete gSteel; delete gAlum;   // 本测试没挂引擎,自己 new 的自己删
}

static void t5_flat() {
    hdr("T5", "ProjectionStages::buildFlat", "flat 落位 —— 直属原位不动,组落在首个成员的原位置");
    World w = makeWorld(false);
    auto* gS = new TreeGroup(GroupTicket{ "Steel", "Steel", 0 }, nullptr);
    auto* gA = new TreeGroup(GroupTicket{ "Alum", "Alum", 0 }, nullptr);
    // vis 顺序:Base BoltA BoltB Cover Sketch;Steel 首成员=Base(位0),Alum 首成员=BoltB(位2)
    std::vector<GuiBase*> vis = { w.base, w.boltA, w.boltB, w.cover, w.sketch };
    // v4:buildFlat 吃"阶段① entries"(只含成员行,container=组;owner 用于判直属)
    std::unordered_map<GuiBase*, PosEntry> entries;
    entries[w.base]   = PosEntry{ gS, 0 };
    entries[w.boltA]  = PosEntry{ gS, 1 };
    entries[w.boltB]  = PosEntry{ gA, 0 };
    entries[w.cover]  = PosEntry{ gA, 1 };
    auto flat = ProjectionStages::buildFlat(vis, entries, w.root);
    showIn("vis = " + lst(vis) + ",  entries = {Base,BoltA→Steel; BoltB,Cover→Alum}");
    showOut("flat = " + lst(flat));
    expectList(flat, { gS, gA, w.sketch }, "5 个节点收敛成 3 行:两组各占首成员位置,Sketch 原位");
    delete gS; delete gA;
    freeWorld(w);
}

static void t6_sort() {
    hdr("T6", "ProjectionStages::sortFlat / sortMembers", "Stage C 排序 —— 组前置,其余按类型秩+名字;stable 不抖动");
    World w = makeWorld(false);
    auto* gS = new TreeGroup(GroupTicket{ "Steel", "Steel", 0 }, nullptr);
    std::vector<GuiBase*> flat = { w.sketch, w.base, gS, w.boltA };
    std::map<TreeGroup*, std::vector<GuiBase*>> members = { { gS, { w.boltA, w.base } } };
    showIn("flat(落位后未排序) = " + lst(flat) + ",  组成员 = [BoltA, Base]");
    ProjectionStages::sortMembers(members, demoLess);
    ProjectionStages::sortFlat(flat, demoLess);
    showOut("flat(排序后) = " + lst(flat) + ",  组成员(排序后) = " + lst(members[gS]));
    expectList(flat, { gS, w.base, w.boltA, w.sketch }, "组最前(秩-1),Part 按名字 Base<BoltA,Sketch 殿后");
    expectList(members[gS], { w.base, w.boltA }, "组内同一比较器排序");
    delete gS;
    freeWorld(w);
}

// ═══════════════ T7~T12:引擎行为 ═══════════════

static void t7_endToEnd() {
    hdr("T7", "TreeProjection::install", "端到端 —— 装一个 Spec,childCount/child 立即给出投影序列");
    World w = makeWorld(true);
    showIn("数据侧 root 孩子 = [Base, BoltA, BoltB, Cover(压制), Sketch1]");
    P.install(w.root, stdSpec());
    auto flat = kids(w.root);
    showOut("childCount(root)=" + std::to_string(P.childCount(w.root)) + ",  序列 = " + lst(flat));
    check(flat.size() == 3, "5 个数据节点 → 3 行(过滤 Cover + 材料分组收敛)");
    check(dynamic_cast<TreeGroup*>(flat[0]) && dynamic_cast<TreeGroup*>(flat[1])
          && flat[2] == w.sketch, "demoLess:组前置(Alum<Steel),Sketch 殿后");
    auto steel = findGroup(w.root, "Steel");
    check(steel && kids(steel) == std::vector<GuiBase*>{ w.base, w.boltA },
          "Steel 组成员 = [Base, BoltA]");
    showOut("child(Steel 组) = " + lst(kids(steel)));
    P.uninstall(w.root);
    check(kids(w.root).size() == 5, "uninstall 后恢复透传(5 行原始顺序)");
    freeWorld(w);
}

static void t8_cache() {
    hdr("T8", "缓存(v4:脏标志)", "数据没变 → 干净页零枚举直接答缓存;变了+通知 → 重算");
    World w = makeWorld(true);
    P.install(w.root, stdSpec());
    auto* steel1 = findGroup(w.root, "Steel");
    kids(w.root);                                     // 再查一次(触发缓存校验)
    auto* steel2 = findGroup(w.root, "Steel");
    showIn("两次 childCount 之间数据零变化");
    showOut("Steel 组指针 第一次=0x" + std::to_string((uintptr_t)steel1)
            + " 第二次=0x" + std::to_string((uintptr_t)steel2));
    check(steel1 == steel2, "活列表没变 → 缓存命中,组指针稳定");
    // 数据侧新增一个零件 → 通知置脏 → 下次读重算,新成员自动进组(dynamicAll)
    auto* rod = new PartObject("Rod", "Steel", w.root);
    w.root->addSubDataGroup(rod);
    P.invalidate(w.root);                              // ★v4 契约:结构变更后必须通知
    auto* steel3 = findGroup(w.root, "Steel");
    showIn("数据侧新增 Rod(Steel) → live 序列变化");
    showOut("Steel 组成员 = " + lst(kids(steel3)));
    check(steel3 == steel1 && P.childCount(steel3) == 3,
          "重算但同 key 组复用(指针不变),Rod 自动进组");
    P.uninstall(w.root);
    freeWorld(w);
}

static void t9_rowOf() {
    hdr("T9", "parentOf / rowOf", "坐标四件套 —— 一切行号出自投影账本,不是数据侧顺序");
    World w = makeWorld(true);
    P.install(w.root, stdSpec());
    auto flat = kids(w.root);                          // [Alum组, Steel组, Sketch1]
    auto* steel = findGroup(w.root, "Steel");
    showIn("投影序列 = " + lst(flat) + ";数据侧顺序是 [Base,BoltA,BoltB,Cover,Sketch]");
    showOut("rowOf(Steel组)=" + std::to_string(P.rowOf(steel))
            + "  rowOf(BoltA)=" + std::to_string(P.rowOf(w.boltA))
            + "  rowOf(Cover)=" + std::to_string(P.rowOf(w.cover))
            + "  parentOf(BoltA)=" + nm(P.parentOf(w.boltA))
            + "  parentOf(Steel组)=" + nm(P.parentOf(steel)));
    check(P.rowOf(steel) == 1 && P.parentOf(steel) == w.root, "Steel 组:root 下第 1 行");
    check(P.parentOf(w.boltA) == steel && P.rowOf(w.boltA) == 1,
          "BoltA:父亲是 Steel 组(不是 root!),组内第 1 行");
    check(P.rowOf(w.cover) == -1, "Cover 被过滤 → rowOf=-1(不在投影树上)");
    P.uninstall(w.root);
    freeWorld(w);
}

static void t10_evaporate() {
    hdr("T10", "空组蒸发", "数据侧删掉组的最后成员 → 下次查询组自动消失(引擎零写数据侧)");
    World w = makeWorld(true);
    P.install(w.root, stdSpec());
    size_t before = P.childCount(w.root);
    size_t groupsBefore = P.liveGroupCount();
    showIn("投影 = " + lst(kids(w.root)) + ",  liveGroupCount=" + std::to_string(groupsBefore));
    P.notifyRemove(w.root, w.boltB);                   // ★v4:先通知(delete 前),再删
    w.root->destroySubDataGroup(w.boltB);              // Alum 组唯一成员
    w.boltB = nullptr;
    auto flat = kids(w.root);                          // 这一问触发重算(页已脏)
    showOut("删 BoltB 后投影 = " + lst(flat) + ",  liveGroupCount=" + std::to_string(P.liveGroupCount()));
    check(flat.size() == before - 1, "root 行数 3→2:Alum 组整组蒸发");
    check(!findGroup(w.root, "Alum") && findGroup(w.root, "Steel"), "Alum 没了,Steel 还在");
    check(P.liveGroupCount() == groupsBefore - 1, "空组退出账本(读侧触发 → 进 m_retired)");
    P.collectGarbage();                                // 无视图场景:安全点回收
    check(P.retiredCount() == 0, "回收后待回收队列清空(无泄漏)");
    P.uninstall(w.root);
    freeWorld(w);
}

static void t11_multiLayer() {
    hdr("T11", "多层分组", "对组再装投影 —— childCount 优先自身投影;parentOf 逐层下钻 deepest-wins");
    World w = makeWorld(false);
    // 第一层:root 按类型分组;第二层:Parts 组内按材料分组
    P.install(w.root, ProjectionSpec{ nullptr,
        GroupRules::byKey([](GuiBase* n) { return n->type(); },
                          [](const std::string& k) { return k + "s"; }),
        demoLess, { false, true } });
    auto* parts = findGroup(w.root, "Part");
    showIn("第一层后 root = " + lst(kids(w.root)));
    P.install(parts, ProjectionSpec{ nullptr, byMaterial(), demoLess, { false, true } });
    showOut("第二层后 childCount(Parts组)=" + std::to_string(P.childCount(parts))
            + ",  Parts组 = " + lst(kids(parts)));
    check(kids(parts).size() == 2, "Parts 组的孩子 = 两个材料组(自身投影优先于外层成员表)");
    auto* steel = findGroup(parts, "Steel");
    check(P.parentOf(w.boltA) == steel, "parentOf(BoltA)=Steel 组(下钻到最深一层)");
    check(P.parentOf(steel) == parts && P.parentOf(parts) == w.root,
          "parentOf 链:Steel→Parts→root(层层都是投影账本的答案)");
    size_t g2 = P.liveGroupCount();
    P.uninstall(w.root);                               // 级联:拆 root 页账时连 Parts 的内层一起拆
    showOut("uninstall(root) 级联后 liveGroupCount:" + std::to_string(g2) + " → "
            + std::to_string(P.liveGroupCount()));
    check(P.liveGroupCount() == 0, "级联拆除:内层材料组没有泄漏");
    freeWorld(w);
}

static void t12_deathOfOwner() {
    hdr("T12", "owner 之死与蒸发", "装过投影的节点被数据侧删除 → 账页变僵尸,但读路径永不 deref 死者");
    World w = makeWorld(false);
    auto* folder = new FolderObject("Box", w.root);
    w.root->addSubDataGroup(folder);
    folder->addSubDataGroup(new PartObject("Nut", "Steel", folder));
    P.install(folder, stdSpec());
    showIn("folder(Box) 装了投影,liveGroupCount=" + std::to_string(P.liveGroupCount()));
    w.root->destroySubDataGroup(folder);               // 没来得及 discard 的"失控死亡"
    auto flat = kids(w.root);                          // 引擎照常工作:folder 从活枚举消失
    showOut("删 folder 后 root 投影 = " + lst(flat) + "(引擎未崩溃,死者不再被枚举)");
    check(flat.size() == 5 && P.childCount(w.root) == 5, "root 行数 6→5(folder 缺席)");
    // 事后补讣告:discard 对已死 key 依然安全(纯 map 操作,绝不 deref 死者)
    P.discard(folder);
    check(P.liveGroupCount() == 0, "discard 清掉死者账页并释放其组(僵尸不隔夜)");
    P.uninstall(w.root);
    freeWorld(w);
}

// ═══════════════ T13~T14:生命周期卫生 ═══════════════

static void t13_prune() {
    hdr("T13", "prune(腿②可达性剪枝)", "从根沿活枚举 DFS,走不到的账页 key=死者 → 清僵尸账+释放孤儿组");
    World w = makeWorld(false);
    auto* folder = new FolderObject("Box", w.root);
    w.root->addSubDataGroup(folder);
    folder->addSubDataGroup(new PartObject("Nut", "Steel", folder));
    P.registerRoot(w.root);
    P.install(folder, stdSpec());
    size_t groupsBefore = P.liveGroupCount();
    w.root->destroySubDataGroup(folder);               // 失控死亡 → folder 账页成僵尸
    showIn("folder 死后:hasProjection(folder)= "
           + std::string(P.hasProjection(folder) ? "true(僵尸)" : "false")
           + ",  liveGroupCount=" + std::to_string(groupsBefore));
    P.prune();
    showOut("prune 后:hasProjection(folder)= "
            + std::string(P.hasProjection(folder) ? "true" : "false(已清)")
            + ",  liveGroupCount=" + std::to_string(P.liveGroupCount()));
    check(!P.hasProjection(folder), "僵尸账页被清掉(地址复用幽灵的根源被拔)");
    check(P.liveGroupCount() < groupsBefore, "孤儿组被释放");
    // ★ 原为 `check(P.hasProjection(w.root) || true, …)` —— 恒真断言,永远测不到东西
    //   (PROJECT.md 缺口清单 #1,已修)。它掩盖的是一个真实事实:
    //   【root 本身没有装投影】(本块只给 folder 装了),所以正确的事实是这三条:
    //     ① prune 不会凭空给谁造账页(root 仍然无);
    //     ② 活着的 5 个数据节点一个不少;
    //     ③ 死掉的 folder 确实不在枚举里(只比指针值,绝不 deref 死者)。
    size_t childrenAfter = P.childCount(w.root);
    std::vector<GuiBase*> alive = kids(w.root);
    bool folderAbsent = true;
    for (GuiBase* n : alive) if (n == folder) folderAbsent = false;
    showOut("prune 后 root: hasProjection="
            + std::string(P.hasProjection(w.root) ? "true" : "false")
            + ", childCount=" + std::to_string(childrenAfter)
            + ", folder 缺席=" + std::string(folderAbsent ? "是" : "否"));
    check(!P.hasProjection(w.root), "★prune 不凭空造账页(root 本就无投影)");
    check(childrenAfter == 5 && alive.size() == 5 && folderAbsent,
          "★活着的 5 个节点一个不少,死掉的 folder 确实缺席");
    P.unregisterRoot(w.root);
    P.uninstall(w.root);
    freeWorld(w);
    // 注:folder 已是死指针,上面只把它当 map key 做值比较,从未 deref —— 这正是 prune 的生存法则
}

static void t14_rebind() {
    hdr("T14", "bindSpec + rebind(腿③声明式绑定)", "投影意图锚定谓词而非指针 → wipe-and-rebuild 后自动重生");
    World w = makeWorld(false);
    auto* origin = new NamedGeom("Origin", nullptr, w.root);
    w.root->addTreeChild(origin);                      // GuiBase 政体:root 连坐 origin
    new NamedGeom("Plane", origin, nullptr);           // QObject 政体:origin 连坐 Plane
    P.registerRoot(w.root);
    P.bindSpec({ "ut-origin",
        [&w](GuiBase* n) {
            auto* g = dynamic_cast<NamedGeom*>(n);
            return g && g->name() == "Origin" && topOf(g) == w.root;
        },
        ProjectionSpec{ nullptr,
                        GroupRules::byPredicate([](GuiBase* n) { return n->type() == "Geom"; }, "GeomSet"),
                        demoLess, { false, true } } });
    showIn("绑定后 childCount(origin)=" + std::to_string(P.childCount(origin))
           + ",  origin = " + lst(kids(origin)));
    check(findGroup(origin, "GeomSet") != nullptr, "GeomSet 组装上了");
    // wipe-and-rebuild:整棵删除重建(GeomObject=纯视图对象的真实机制)
    w.root->removeTreeChild(origin);
    delete origin;
    P.prune();                                         // ★ 必做:清僵尸账
    auto* origin2 = new NamedGeom("Origin", nullptr, w.root);
    w.root->addTreeChild(origin2);
    new NamedGeom("Plane", origin2, nullptr);
    P.rebind();                                        // ★ 谓词重新命中新 Origin
    showOut("重建+rebind 后 childCount(origin2)=" + std::to_string(P.childCount(origin2))
            + ",  origin2 = " + lst(kids(origin2)));
    check(findGroup(origin2, "GeomSet") != nullptr, "GeomSet 组在新 Origin 上重生(重建代码零感知)");
    P.unbindSpec("ut-origin");
    P.unregisterRoot(w.root);
    P.uninstall(origin2);
    P.uninstall(w.root);
    freeWorld(w);                                      // ~GuiBase(root) 连坐 origin2;~QObject 连坐 Plane
}

// ═══════════════ T15~T16:模型层 ═══════════════

#ifndef NT_ENGINE_ONLY
static NavTreeModel* m15 = nullptr;   // 只给 t15 的递归 walk 用(避免 lambda 套娃传参)
#endif

#ifndef NT_ENGINE_ONLY
static void t15_model() {
    int n = 0;
    bool ok = true;
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& idx) {
        int rows = m15->rowCount(idx);
        for (int r = 0; r < rows; ++r) {
            QModelIndex c = m15->index(r, 0, idx);
            if (!c.isValid() || c.parent() != idx) { ok = false; return; }
            if (m15->data(c, Qt::DisplayRole).toString().isEmpty()) { ok = false; return; }
            ++n;
            walk(c);
        }
    };
    hdr("T15", "NavTreeModel 三要素", "index/parent/rowCount 全部出自投影账本 → 全树自洽");
    World w = makeWorld(true);
    P.install(w.root, stdSpec());
    NavTreeModel model(w.root, P);
    m15 = &model;
    model.setLabeler([](GuiBase* x) { return QString::fromStdString(nm(x)); });
    showIn("root 装好 stdSpec,模型挂在引擎的 ITreeSource 窄接口上");
    walk(QModelIndex());
    showOut("递归校验节点数 = " + std::to_string(n)
            + ",  parent(index(c))==idx 与 data 非空全过 = " + (ok ? "true" : "false"));
    check(ok && n == 6, "root 3 行 + Steel 组 2 成员 + Alum 组 1 成员 = 6 个节点全自洽");
    m15 = nullptr;
    P.uninstall(w.root);
    freeWorld(w);
}
#endif // !NT_ENGINE_ONLY

#ifndef NT_ENGINE_ONLY
static void t16_signals() {
    hdr("T16", "事前坐标删除协议", "死前问 parentOf/rowOf → begin → 删 → end;信号恰发一次,坐标是投影行号");
    World w = makeWorld(true);
    P.install(w.root, stdSpec());
    NavTreeModel model(w.root, P);
    int removed = 0, inserted = 0;
    QObject::connect(&model, &QAbstractItemModel::rowsAboutToBeRemoved,
                     [&](const QModelIndex&, int, int) { ++removed; });
    QObject::connect(&model, &QAbstractItemModel::rowsAboutToBeInserted,
                     [&](const QModelIndex&, int, int) { ++inserted; });

    // ① 组内成员:1 条信号,坐标在组层
    GuiBase* par = P.parentOf(w.boltA);
    int row = P.rowOf(w.boltA);
    showIn("BoltA 死前坐标:parentOf=" + nm(par) + ", rowOf=" + std::to_string(row));
    model.beginRowRemoval(par, row);
    P.notifyRemove(w.root, w.boltA);                   // ★v4:通知在 delete 前(begin/end 括号内)
    w.root->destroySubDataGroup(w.boltA);
    model.endRowRemoval();
    showOut("信号计数 removed=" + std::to_string(removed));
    check(removed == 1 && P.childCount(par) == 1, "1 条 removeRows@组层,Steel 组 2→1");

    // ② 组的最后成员:级联预判 → 信号发在组行(root 层 1 条)
    par = P.parentOf(w.boltB);
    bool last = P.isGroup(par) && P.childCount(par) == 1;
    int grow = P.rowOf(par);
    showIn("BoltB 是 Alum 组最后成员=" + std::string(last ? "true" : "false")
           + " → 改报组坐标 rowOf(Alum组)=" + std::to_string(grow));
    model.beginRowRemoval(P.parentOf(par), grow);
    P.notifyRemove(w.root, w.boltB);                   // ★v4 通知
    w.root->destroySubDataGroup(w.boltB);
    model.endRowRemoval();
    showOut("信号计数 removed=" + std::to_string(removed));
    check(removed == 2 && !findGroup(w.root, "Alum"), "整组蒸发只发 root 层 1 条(累计 2)");

    // ③ 被过滤节点:rowOf=-1 → 零信号(但它仍在【原始】孩子列表里 → 通知照样要给)
    check(P.rowOf(w.cover) == -1, "Cover 死前 rowOf=-1(不在投影树上)");
    P.notifyRemove(w.root, w.cover);                   // ★v4:被过滤≠不用通知(它占着 lastLive)
    w.root->destroySubDataGroup(w.cover);              // 无需任何视图信号
    showOut("删 Cover 后 removed=" + std::to_string(removed) + "(未增加)");
    check(removed == 2 && inserted == 0, "零信号 = 正确(视图上本来就没有这一行)");
    P.uninstall(w.root);
    P.collectGarbage();
    freeWorld(w);
}
#endif // !NT_ENGINE_ONLY

// ═══════════════ T17~T18:契约回归 + 模型协议 ═══════════════

// T17:读侧不释放组(坑31 的持续回归)
//   这是外部 review 用探针实证过的 UAF 契约漏洞:裸删成员后只问一句
//   childCount(root)(= Qt 的 rowCount()),组就被 delete 掉了,而视图手里
//   的 QPersistentModelIndex.internalPointer 正是那个 TreeGroup*。
//   修法:m_retired 待回收队列 + collectGarbage() 在安全点统一释放。
//   本块把"修复后的四条可观测事实"冻结成断言,防止将来回退。
static void t17_readSideRelease() {
    hdr("T17", "读侧不释放组(m_retired 契约)",
        "只读查询淘汰组 → 只摘账入队,不 delete;collectGarbage() 才真正释放");
    World w = makeWorld(false);            // 不过滤:Alum 组将含 BoltB + Cover
    P.install(w.root, stdSpec());
    size_t groupsBefore = P.liveGroupCount();

    GuiBase* alum = findGroup(w.root, "Alum");
    const void* addrBefore = alum;
    showIn("Alum 组合 " + std::to_string(alum ? P.childCount(alum) : 0)
           + " 个成员;liveGroupCount=" + std::to_string(groupsBefore));
    size_t retiredBefore = P.retiredCount();

    // 数据侧删 Alum 的全部成员(v4:通知后删 —— 本块要验的是"淘汰由【只读查询】
    // 触发时,组只进 m_retired 不 delete";v3 时代这里是零通知裸删,靠枚举发现,
    // v4 的发现机制换成了通知,契约主体不变:触发淘汰的仍是一句纯读)
    P.notifyRemove(w.root, w.boltB);
    w.root->destroySubDataGroup(w.boltB);
    P.notifyRemove(w.root, w.cover);
    w.root->destroySubDataGroup(w.cover);
    w.boltB = w.cover = nullptr;

    // ★ 关键:只问一句【只读查询】
    size_t rows = P.childCount(w.root);
    showOut("裸删成员后,只调一次 P.childCount(root)(= Qt rowCount())= "
            + std::to_string(rows)
            + ";  retiredCount " + std::to_string(retiredBefore)
            + " -> " + std::to_string(P.retiredCount())
            + ";  liveGroupCount=" + std::to_string(P.liveGroupCount()));
    check(findGroup(w.root, "Alum") == nullptr, "只读查询后组已从投影账本摘除");
    check(P.liveGroupCount() == groupsBefore - 1, "存活组数 -1(Alum 组退出投影)");
    check(P.retiredCount() > retiredBefore, "★组进 m_retired【而非被 delete】(修 UAF)");

    // 地址未被复用:回收前旧组内存仍被占用 → 旧 internalPointer 不会指向别人
    w.root->addSubDataGroup(new PartObject("BoltB2", "Alum", w.root));
    P.invalidate(w.root);                              // ★v4:新增也要通知
    w.boltB = nullptr;
    P.childCount(w.root);
    GuiBase* alum2 = findGroup(w.root, "Alum");
    showOut("补回一个 Alum 成员后组重生:addr " + std::to_string((size_t)addrBefore)
            + " -> " + std::to_string((size_t)(void*)alum2));
    check(alum2 != nullptr && (const void*)alum2 != addrBefore,
          "★组地址未被复用(回收前旧内存仍被占用,幽灵无处寄生)");

    // 安全点回收:collectGarbage() 后队列清空
    P.collectGarbage();
    showOut("collectGarbage() 后 retiredCount=" + std::to_string(P.retiredCount()));
    check(P.retiredCount() == 0, "collectGarbage() 后待回收队列清空(无泄漏)");

    P.uninstall(w.root);
    P.collectGarbage();
    freeWorld(w);
}

// T18:模型协议 + 信号正确性(★分成两件不同的事,别混为一谈)
//   ⚠⚠ 本块原有的那句论断"Tester 正好覆盖'读查询改结构 / 信号发错行'"是【错的】,
//      实测(故意注入级联预判错误)证明 Tester 根本抓不到 —— 原因:
//        模型的 rowCount/index/parent 每次都重新问【引擎】拿当前真实结构,
//        所以"模型报告的结构"永远自洽。Tester 查的是"模型自己的账对不对得上",
//        而"发出的信号与实际结构变化是否匹配"完全不在它的检查范围内。
//        那个注入实验的证据:诊断打印 isGroup(par)=1 childCount=1(组必蒸发),
//        Tester 仍报"全程无断言 = true"。
//   所以这里拆成两段,各用各的工具:
//     ① Tester 查它擅长的:index/parent/rowCount/data 的互逆性、信号配对。
//     ② 捕获 rowsAboutToBeRemoved 的【参数】,断言信号发在正确的 (parent,row)。
//        ⚠ 千万别用"删除前后 rowCount 差值"来验证 —— 我第一版就是这么写的,
//        而且它【抓不到任何东西】:rowCount() 每次都重新问引擎拿真实结构,
//        差值永远是真实减少量,和信号发没发对毫无关系(实测反例照样"通过")。
//        要验证信号,就得看信号【本身】的参数。
#ifndef NT_ENGINE_ONLY
static void t18_modelProtocol() {
    hdr("T18", "模型协议(Tester)+ 信号坐标(rowsAboutToBeRemoved 参数)",
        "Tester 查模型账本自洽;信号参数断言查'发在哪一行'(Tester 查不了)");
    World w = makeWorld(true);
    P.install(w.root, stdSpec());
    NavTreeModel model(w.root, P);
    model.setLabeler([](GuiBase* x) { return QString::fromStdString(nm(x)); });
    showIn("root 装 stdSpec,模型挂 ITreeSource;Tester 用 FailureReportingMode::Fatal");

    bool ok = true;
    {
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
        // 主动遍历一次,把 index/parent/rowCount/data 全部喂给 Tester 检查
        std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& idx) {
            int rows = model.rowCount(idx);
            for (int r = 0; r < rows; ++r) {
                QModelIndex c = model.index(r, 0, idx);
                if (!c.isValid() || c.parent() != idx) { ok = false; return; }
                (void)model.data(c, Qt::DisplayRole);
                walk(c);
            }
        };
        walk(QModelIndex());
        // ① Tester 段:一次普通成员删除(Steel 组删掉 BoltA 后还有 Base,
        //    不蒸发)—— 这条走成员行路径,组存活,只让 Tester 查协议自洽。
        GuiBase* par = P.parentOf(w.boltA);
        model.beginRowRemoval(par, P.rowOf(w.boltA));
        P.notifyRemove(w.root, w.boltA);               // ★v4 通知
        w.root->destroySubDataGroup(w.boltA);
        w.boltA = nullptr;
        model.endRowRemoval();
        walk(QModelIndex());
    }   // Tester 析构前若发现协议问题会直接以 Fatal 报出
    showOut("① Tester 段:遍历 + 普通成员删除,全程无断言,账本自洽 = "
            + std::string(ok ? "true" : "false"));
    check(ok, "★① QAbstractItemModelTester 全绿(模型账本自洽)");

    // ② 信号坐标段 —— Tester 查不了的那部分。
    //   makeWorld(true) 下 Cover 被过滤 ⇒ Alum 组只有 BoltB 一个成员 ⇒
    //   删掉 BoltB 会让【整组蒸发】,正确坐标是【组行】而不是组内成员行。
    //   这正是"级联预判"要处理的情形(坑34),也是原来完全没被测试覆盖的那条。
    {
        // 捕获信号参数:rowsAboutToBeRemoved 在 beginRemoveRows 时就发出,
        //   所以我们主动连一次就能拿到"这次到底宣称要删哪个 (parent,row)"。
        QModelIndex sigParent; int sigFirst = -1, sigLast = -2, sigCount = 0;
        auto conn = QObject::connect(&model, &QAbstractItemModel::rowsAboutToBeRemoved,
            [&](const QModelIndex& p, int f, int l) {
                if (sigCount == 0) { sigParent = p; sigFirst = f; sigLast = l; }
                ++sigCount;
            });

        int rowsBefore = model.rowCount(QModelIndex());
        GuiBase* boltB = w.boltB;
        GuiBase* parB = P.parentOf(boltB);
        bool willEvaporate = P.isGroup(parB) && P.childCount(parB) == 1;
        showIn("② Alum 组成员数=" + std::to_string(P.childCount(parB))
               + " → 删它会整组蒸发=" + std::string(willEvaporate ? "true" : "false")
               + ";root 行数=" + std::to_string(rowsBefore));
        check(willEvaporate, "② 前置:Alum 组只剩一个成员(级联预判该生效)");

        // ── 反例对照:证明这个断言【有分辨力】(不发信号,零风险)─────────
        //   错误做法会用"组内成员行"坐标。⚠ 千万别只比 row 数值 ——
        //   实测这个场景里组内成员行号与组行号【恰好都是 0】,数值比较会漏判。
        //   决定性的区分点是【parent】:错误做法把信号发在"组"下面,
        //   正确做法发在组的【父】下面,两者必然不同。
        int wrongRow  = P.rowOf(boltB);                  // ✗ 错误:组内成员的行
        GuiBase* wrongParent = P.parentOf(boltB);        // ✗ 错误:组本身
        GuiBase* cas  = cascadeTarget(P, boltB);
        int rightRow  = cas ? P.rowOf(cas) : -1;
        GuiBase* rightParent = cas ? P.parentOf(cas) : nullptr;
        showOut("   错误坐标=(parent=" + nm(wrongParent) + ", row=" + std::to_string(wrongRow)
                + ")  正确坐标=(parent=" + nm(rightParent) + ", row=" + std::to_string(rightRow) + ")");
        check(cas == parB && wrongParent != rightParent,
              "★② 反例对照:错误坐标的 parent 是【组】、正确的是【组的父】"
              "=> 本断言有分辨力(且不依赖 row 数值,行号可能巧合相等)");

        // ── 正例:用 cascadeTarget 发信号,验证参数确实是"组行" ──
        model.beginRowRemoval(rightParent, rightRow);
        P.notifyRemove(w.root, w.boltB);               // ★v4 通知
        w.root->destroySubDataGroup(w.boltB);
        w.boltB = nullptr;
        model.endRowRemoval();

        QModelIndex expectParent = model.indexForParent(rightParent);
        showOut("   正例:发信号 (parent=" + QString::fromStdString(nm(rightParent)).toStdString()
                + ", row=" + std::to_string(rightRow) + "),收到 (parent="
                + (sigParent == expectParent ? "匹配" : "不匹配")
                + ", first=" + std::to_string(sigFirst) + ")共 "
                + std::to_string(sigCount) + " 条");
        check(sigCount == 1, "★② 恰好发一条 rowsAboutToBeRowsRemoved(组整行消失)");
        check(sigParent == expectParent && sigFirst == rightRow && sigLast == rightRow,
              "★② 信号发在【最外层组行】(parent 与 row 都对上)");
        QObject::disconnect(conn);
    }

    P.uninstall(w.root);
    P.collectGarbage();
    freeWorld(w);
}
#endif // !NT_ENGINE_ONLY

// T19:级联预判的多层覆盖(坑34 的专项回归)
//   坑34 的失效形状是【多层】:"内层组蒸发 → 外层组也跟着空 → 外层也蒸发"。
//   单层场景(一条组直接挂在 root 下)测不到它 —— 原 T18 删的是 BoltA,
//   而 Steel = {Base, BoltA},删完还有 Base,所以历史上这条路径零覆盖。
//   本块造一个【两层嵌套】root → Outer → Inner → Only。
//   ★ 构造要点(照 demo 的 installGrouping 范式,不是随便写个 rule 就行):
//     分组规则作用在 parts = 【vis 数据节点】+ 【旧组】 上 —— 组能被"收进桶"
//     靠的是旧组参与 parts,而不是 rawChildren 里出现组(组的成员表里没有组)。
//     所以要分两步:先让数据节点成组(Inner),再让规则把"名为 Inner 的组"收进
//     另一个桶(Outer)。且规则必须【只收 key==Inner 的组】,否则 Outer 会把自己
//     也收进去 → 无限套娃(工厂规则无条件豁免组节点正是为了防这个)。
#ifndef NT_ENGINE_ONLY
static void t19_cascadeMultiLayer() {
    hdr("T19", "级联预判·多层(内层蒸发→外层也跟着空)",
        "两层嵌套下删最内层唯一成员:cascadeTarget 必须返回最外层组,且信号只发一条");
    // 自包含小树:不去依赖其它块的中间状态(前车之鉴:Q 电池曾因此测自己)
    auto* root = new NamedData("R");
    auto* part = new PartObject("Only", "M", root);
    root->addSubDataGroup(part);

    // 一步到位:数据节点 → Inner;名为 Inner 的【组】→ Outer;其它(Outer 自己)→ 不收
    ProjectionSpec spec;
    spec.grouping = [](GuiBase* n) -> std::optional<GroupTicket> {
        if (auto* g = dynamic_cast<TreeGroup*>(n)) {
            if (g->ticket().key != "Inner") return std::nullopt;   // ★别把 Outer 收进自己
            GroupTicket t; t.key = "Outer"; t.label = "Outer"; return t;
        }
        if (dynamic_cast<PartObject*>(n)) {
            GroupTicket t; t.key = "Inner"; t.label = "Inner"; return t;
        }
        return std::nullopt;
    };
    spec.opt.dynamicAll = true;

    NavTreeModel model(root, P);
    model.setLabeler([](GuiBase* x) { return QString::fromStdString(nm(x)); });

    // 第一次 install:此时没有组 → 只建出 Inner
    P.install(root, spec, {});
    TreeGroup* inner = findGroup(root, "Inner");
    check(inner != nullptr, "T19 前置①:第一次 install 建出内层组 Inner");
    // 第二次 install:Inner 现在作为旧组参与 parts → 被收进 Outer
    P.install(root, spec, {});
    TreeGroup* outer = findGroup(root, "Outer");
    check(outer != nullptr, "T19 前置②:第二次 install 把 Inner 收进 Outer(两层嵌套)");
    if (!outer) { check(false, "T19 中止:Outer 不存在"); P.uninstall(root); P.collectGarbage(); delete root; return; }
    // ★ 从【数据往上】问投影父亲链,而不是从 Outer 往下找 —— parentOf(outer) 是
    //   "Outer 的父亲"(= root),方向反了。而且 Inner 已被收进 Outer,
    //   按名字扫一层找不到它(这个坑我踩过两次:Q 电池、T19 各一次)。
    GuiBase* iNode = P.parentOf(part);                        // Only 的投影父亲 = Inner
    GuiBase* oNode = iNode ? P.parentOf(iNode) : nullptr;      // Inner 的投影父亲 = Outer
    TreeGroup* inner2 = dynamic_cast<TreeGroup*>(iNode);
    check(inner2 != nullptr && dynamic_cast<TreeGroup*>(oNode) == outer,
          "T19 前置③:投影父亲链逐层正确 Only→Inner→Outer(parentOf 逐层下钻)");
    if (!inner2 || dynamic_cast<TreeGroup*>(oNode) != outer) {
        check(false, "T19 中止:层级链不符合预期");
        P.uninstall(root); P.collectGarbage(); delete root; return;
    }
    // 结构自检:root → Outer → Inner → Only
    check(P.parentOf(outer) == root && P.parentOf(inner2) == outer
          && P.parentOf(part) == inner2 && P.childCount(outer) == 1,
          "T19 结构: root→Outer→Inner→Only 四层坐标逐层正确");

    // ★ 核心:删最内层的唯一成员 → Inner 蒸发 → Outer 也空 → Outer 也蒸发。
    //   信号必须发在【Outer】的位置,root 层恰好掉 1 行。
    int before = model.rowCount(QModelIndex());
    GuiBase* cas = cascadeTarget(P, part);
    showIn("两层: cascadeTarget(Only) = " + nm(cas) + "(期望 Outer,不能是 Inner)");
    check(cas == outer, "★cascadeTarget 返回最外层组 Outer(不是内层组 Inner)");

    // ★ 分辨力自检:单层判法(旧版做法)在这里【必然】答 Inner。
    //   若哪天 cascadeTarget 退化成单层,本行会立刻变红 —— 证明上面那条
    //   真的能区分"多层正确"与"单层错误",不是恒绿的摆设。
    check(inner2 != nullptr && inner2 != cas,
          "★分辨力自检:单层判法会答 Inner,而正确答案(最外层)是 Outer —— 本断言非恒绿");

    QModelIndex sigParent; int sigFirst = -1, sigCount = 0;
    auto conn = QObject::connect(&model, &QAbstractItemModel::rowsAboutToBeRemoved,
        [&](const QModelIndex& p, int f, int) { if (!sigCount) { sigParent = p; sigFirst = f; } ++sigCount; });

    model.beginRowRemoval(P.parentOf(cas), P.rowOf(cas));
    P.notifyRemove(root, part);                        // ★v4 通知(delete 前)
    root->destroySubDataGroup(part);
    model.endRowRemoval();

    int after = model.rowCount(QModelIndex());
    showOut("删除内层唯一成员后: root 行数 " + std::to_string(before) + "→"
            + std::to_string(after) + "(掉 " + std::to_string(before - after)
            + " 行);信号 " + std::to_string(sigCount) + " 条 @row " + std::to_string(sigFirst));
    check(sigCount == 1, "★两层级联蒸发:只发一条信号(两层一起消失也只需一条)");
    check(before - after == 1, "★两层级联:恰好掉 1 行");
    check(findGroup(root, "Outer") == nullptr && findGroup(root, "Inner") == nullptr,
          "★两层都没留下僵尸行");
    QObject::disconnect(conn);

    P.uninstall(root);
    P.collectGarbage();
    delete root;
}
#endif // !NT_ENGINE_ONLY

// T21:宽节点读侧(防 O(k²) 回退)
//   背景(2026-10-06):模型对每一行都问 parentOf/rowOf,而旧实现为了给出答案必须
//   revalidate → 全枚举该页孩子 ⇒ 遍历 k 行 = k 次 O(k) ⇒ O(k²);
//   索引化后 parentOf/rowOf 变成"每页每纪元最多校验一次 + 查表"。
//   实测(Release,独立探针):最宽层 5000 时一次全树遍历 76 ms → 0.53 ms;
//   2000 个成员逐个取(组路径)14.7 s → 0.28 µs/次。
//   ★ 本块规模取 600、阈值 2000 ms:O(k²) 实现约 3 ms 也会超?
//     不会 —— 所以这里的价值是"钉住"而非"精确测速":它保证最坏情况下不会
//     退化到 k≥2000 那种秒级行为,同时让 Debug(无优化)也能秒过。
static void t21_wideReadCost() {
    hdr("T21", "宽节点读侧(防 O(k²) 回退)",
        "600 个直属孩子的页:全树 parentOf/rowOf 一遍不得退化成秒级");
    const int N = 600;
    auto* root = new NamedData("WideRoot");
    auto* flat = new FolderObject("宽父亲", root);
    root->addSubDataGroup(flat);
    for (int i = 0; i < N; ++i)
        flat->addSubDataGroup(new PartObject("W" + std::to_string(i), (i % 2) ? "Steel" : "Alum", flat));
    P.registerRoot(root);
    ProjectionSpec sp;
    sp.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* p = dynamic_cast<PartObject*>(n);
        return p ? p->material() : std::string();
    });
    sp.opt.dynamicAll = true;
    P.install(flat, sp, {});
    showIn("宽父亲 = " + std::to_string(N) + " 个零件,按材料分 2 组;先量全树遍历");

    // 收集投影树全部节点
    std::vector<GuiBase*> nodes;
    std::function<void(GuiBase*)> collect = [&](GuiBase* n) {
        for (size_t i = 0, c = P.childCount(n); i < c; ++i) {
            GuiBase* k = P.child(n, i);
            if (!k) continue;
            nodes.push_back(k); collect(k);
        }
    };
    collect(root);

    // 正确性(与性能同块):全树不变量必须成立
    bool ok = true;
    std::function<void(GuiBase*)> inv = [&](GuiBase* n) {
        for (size_t i = 0, c = P.childCount(n); i < c && ok; ++i) {
            GuiBase* k = P.child(n, i);
            if (!k || P.parentOf(k) != n || P.rowOf(k) != (int)i) { ok = false; return; }
            inv(k);
        }
    };
    inv(root);

    const auto t0 = std::chrono::steady_clock::now();
    for (GuiBase* n : nodes) { (void)P.parentOf(n); (void)P.rowOf(n); }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    showOut("投影节点 " + std::to_string(nodes.size()) + " 个,全树 parentOf+rowOf = "
            + std::to_string(ms) + " ms");
    check(ok, "T21 前置:宽节点全树不变量成立(索引化不能改答案)");
    // ★ 阈值分配置(评审修复 R1 之后必须如此,2026-10-06):
    //   R1 恢复了"每个操作必重枚举"的跨调用新鲜度(parentOf/rowOf 快路径之前开纪元)——
    //   这是正确性要求(裸坐标查询吃过陈旧账本,探针 P1 实证)。代价:遍历 k 行
    //   回到 O(k×页宽) 的诚实成本(PROJECT.md §3 本就声明跨调用 O(k²) 未消除)。
    //   本页是 DataObject 政体(每次 visibleChild 都 O(n) 重填)→ Debug 无优化下
    //   603 次查询 ≈ 13 s,是语义的必然成本,不是退化。
    //   Release 阈值 2 s 不变;Debug 阈值 30 s —— 仍能抓住真爆炸
    //   (若有人把 revalidate 塞进逐元素循环,这里会 ×600 → 远超任何阈值)。
#ifdef _DEBUG
    const double limit = 30000.0;
#else
    const double limit = 2000.0;
#endif
    check(ms < limit, "★宽节点读侧无病态退化(" + std::to_string(ms) + " ms < "
          + std::to_string(limit) + " ms,阈值分配置)");

    P.uninstall(root);
    P.unregisterRoot(root);
    P.collectGarbage();
    clearData(root);
    delete root;
}

// ═══════════════ main ═══════════════

// ★ Debug CRT 的弹窗必须关掉(三条理由,都是实测踩出来的):
//   ① 堆损坏报告会以 MessageBox 阻塞 —— 自检程序就此卡死,"退出码=失败数"
//      的契约彻底失效(用户会看到弹框而不是测试结果);
//   ② 弹框盖住真正的失败现场,输出被截断,反而更难查;
//   ③ _CrtSetReportMode 的首参是【单一报告类型】而非位掩码 —— 传
//      (_CRT_ASSERT|_CRT_ERROR|...) 会让 CRT 反过来断言自己的参数,又弹一次。
//   正确做法:逐类型设置 + SetErrorMode 禁系统弹框,报告写 stderr(下面重定向到文件)。
#ifdef _MSC_VER
static void silenceCrtDialogs() {
#ifdef Q_OS_WIN
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, stderr);
    _CrtSetReportFile(_CRT_ERROR,  stderr);
    _CrtSetReportFile(_CRT_WARN,   stderr);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}
#endif // _MSC_VER

// T20(v4):漏通知的检测与自愈(paranoid + prune 两层兜底)
//   v4 契约:数据变更后调用方【必须】通知(invalidate/notifyRemove);
//   漏通知不崩溃,只投影陈旧,并由两层兜底收敛:
//     ① paranoid(默认 Debug 开):childCount 对干净页额外比对 lastLive,
//        漂移 → stderr 点名 + 当场自愈(开发期一跑就暴露漏报点);
//     ② prune(双配置都跑):卫生边界全量自愈 —— "忘了通知,陈旧最多活到下一次 prune"。
//   分辨力:去掉 healIfDrift 的任何一层,对应断言必红。
static void t20_paranoidAndHeal() {
    hdr("T20", "漏通知检测与自愈(paranoid + prune)",
        "静默删除后:paranoid 开=childCount 当场自愈;paranoid 关=陈旧到 prune 为止");
    auto* root = new NamedData("R20");
    auto* flat = new FolderObject("页", root);
    root->addSubDataGroup(flat);
    std::vector<PartObject*> lv;
    for (int i = 0; i < 4; ++i) {
        auto* l = new PartObject("L" + std::to_string(i), (i % 2) ? "B" : "A", flat);
        flat->addSubDataGroup(l);
        lv.push_back(l);
    }
    P.registerRoot(root);
    ProjectionSpec sp;
    sp.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* p = dynamic_cast<PartObject*>(n);
        return p ? p->material() : std::string();
    });
    sp.opt.dynamicAll = true;
    P.install(flat, sp, {});            // GA=[L0,L2]  GB=[L1,L3]
    check(P.childCount(flat) == 2, "① 前置:两组");

    // ── ① paranoid 开:静默删除(不通知)→ childCount 当场自愈 ──
    bool oldParanoid = P.paranoidChecks();
    P.setParanoidChecks(true);
    flat->destroySubDataGroup(lv[0]);   // 静默删 L0(GA 成员),引擎未收到任何通知
    flat->destroySubDataGroup(lv[2]);   // 静默删 L2 → GA 应蒸发
    size_t cc = P.childCount(flat);     // paranoid 比对发现漂移 → 置脏 → 重算
    showOut("paranoid 开,静默删光 GA 后 childCount = " + std::to_string(cc) + "(真值 1)");
    check(cc == 1, "★① paranoid:漏通知被 childCount 当场自愈(GA 蒸发,只剩 GB)");
    P.collectGarbage();                 // 无视图场景,直接回收淘汰组

    // ── ② paranoid 关:静默删除 → 允许陈旧,prune 兜底收敛 ──
    P.setParanoidChecks(false);
    flat->destroySubDataGroup(lv[1]);   // 静默删 L1
    flat->destroySubDataGroup(lv[3]);   // 静默删 L3 → GB 实际已空
    size_t stale = P.childCount(flat);  // 干净页零枚举 → 答陈旧 flat
    showOut("paranoid 关,静默删光 GB 后 childCount = " + std::to_string(stale)
            + "(陈旧值 1,真值 0 —— 契约允许)");
    check(stale == 1, "② paranoid 关:漏通知 → 投影陈旧(这是契约,不是 bug)");
    P.prune();                          // 卫生边界:全量自愈
    size_t healed = P.childCount(flat);
    check(healed == 0, "★② prune 自愈:陈旧页在卫生边界收敛(GB 蒸发)");
    P.collectGarbage();

    P.setParanoidChecks(oldParanoid);
    P.uninstall(flat);
    P.unregisterRoot(root);
    P.collectGarbage();
    clearData(root);
    delete root;
}

// T22:裸坐标查询的新鲜度(评审修复 R1 的回归,2026-10-06)
//   病灶:parentOf/rowOf 的快路径曾在 EpochScope 之前执行 —— 裸调用不递增纪元,
//   快路径内 revalidate 的闸门把"上一操作校验过"误当"本操作校验过",
//   数据侧静默变更后返回【陈旧坐标】。探针实证(probe_review P1):
//   删 L1 后裸 rowOf(L3) 答 1(真值 0)。修复 = 纪元在快路径之前开。
//   分辨力:去掉 parentOf/rowOf 开头的 EpochScope,本块第 2 条断言必红(答 1)。
static void t22_bareCoordFreshness() {
    hdr("T22", "裸坐标查询新鲜度(R1 回归)",
        "数据侧静默删除后,不经 childCount 直接 parentOf/rowOf 也必须给新鲜答案");
    auto* root = new NamedData("R22");
    auto* flat = new FolderObject("页", root);
    root->addSubDataGroup(flat);
    // ⚠ DataObject 构造只记反向指针,【不】自动挂进 sup 的 m_subDataGroups
    //   —— 必须显式 addSubDataGroup,否则节点根本不在树上(第一版就漏了 L1/L3)
    auto* l1 = new PartObject("L1", "A", flat);
    flat->addSubDataGroup(l1);
    auto* l2 = new PartObject("L2", "B", flat);
    flat->addSubDataGroup(l2);
    auto* l3 = new PartObject("L3", "A", flat);
    flat->addSubDataGroup(l3);
    auto* l4 = new PartObject("L4", "B", flat);
    flat->addSubDataGroup(l4);
    P.registerRoot(root);
    ProjectionSpec sp;
    sp.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* p = dynamic_cast<PartObject*>(n);
        return p ? p->material() : std::string();
    });
    sp.opt.dynamicAll = true;
    P.install(flat, sp, {});            // GA=[L1,L3]  GB=[L2,L4]
    showIn("页装投影(材料 2 组);先 childCount 校验建索引,再静默删 L1");

    (void)P.childCount(flat);           // op1:重算 + entries 登记(L3 → GA 行 1)
    check(P.rowOf(l3) == 1, "① 前置:GA=[L1,L3],L3 行号 = 1");

    // v4 契约:删除 → notifyRemove(必须在 delete 前;= invalidate(页)+discard(死者))
    P.notifyRemove(flat, l1);
    flat->destroySubDataGroup(l1);

    // ★ 关键断言:下一个引擎调用是【裸 rowOf】—— 通知过后必须立即新鲜,
    //   不需要任何 childCount 预热(v3 的 R1 病灶:快路径跳过校验吃陈旧账)
    int r = P.rowOf(l3);
    showOut("notifyRemove+删 L1 后裸 rowOf(L3) = " + std::to_string(r) + "(真值 0,陈旧值 1)");
    check(r == 0, "★② 裸 rowOf(L3)=0:通知即新鲜,无需预热查询(v4 契约)");
    GuiBase* par = P.parentOf(l3);      // 裸 parentOf 同样必须新鲜
    check(par && P.isGroup(par) && P.childCount(par) == 1,
          "③ 裸 parentOf(L3)=GA 且 GA 只剩 1 员");

    P.uninstall(flat);
    P.unregisterRoot(root);
    P.collectGarbage();
    clearData(root);
    delete root;
}

// T23:拆页时 m_nodePage 位置索引的清理(评审修复 R2 的回归,2026-10-06)
//   病灶:dropProjection 的清理键集曾收 flat + members 的【组键】,漏掉各组
//   【成员】—— 与登记键集(memberToPos 的键)不对称,成员条目永久残留。
//   探针实证(probe_review P2):6 条只清 2 条。答案正确性靠快路径查空回退兜底,
//   但 m_nodePage 随组变更单调积累 = 内存泄漏。修复 = 清理直接遍历 memberToPos 键。
//   分辨力:把清理循环换回旧写法(flat+组键),本块断言必红(差值 2 ≠ 6)。
static void t23_nodePageCleanup() {
    hdr("T23", "m_nodePage 拆页清理(R2 回归)",
        "uninstall 后本页位置索引条目(2 组 + 4 成员 = 6 条)必须全部清除");
    auto* root = new NamedData("R23");
    auto* flat = new FolderObject("页", root);
    root->addSubDataGroup(flat);
    for (int i = 0; i < 4; ++i)
        flat->addSubDataGroup(new PartObject("L" + std::to_string(i), (i % 2) ? "B" : "A", flat));
    P.registerRoot(root);
    ProjectionSpec sp;
    sp.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* p = dynamic_cast<PartObject*>(n);
        return p ? p->material() : std::string();
    });
    sp.opt.dynamicAll = true;
    P.install(flat, sp, {});
    (void)P.childCount(flat);           // 建索引:flat 2 组 + 4 成员 = entries 6 条
    size_t before = P.memoryBreakdown().entries;
    showIn("页装投影并建索引,entries 基线 = " + std::to_string(before));

    P.uninstall(flat);
    size_t after = P.memoryBreakdown().entries;
    showOut("uninstall 后 entries = " + std::to_string(after) + "(应减少 6)");
    check(before >= 6 && after == before - 6,
          "★拆页清掉全部 6 条位置索引(v4:entries 随页析构,结构上不可能泄漏)");

    P.unregisterRoot(root);
    P.collectGarbage();
    clearData(root);
    delete root;
}

// T24:空壳组 —— 规则输出【自相矛盾】的专项回归
//   场景:"不再有任何节点属于 Inner" 与 "Inner 属于 Outer" 同时成立。
//   后者来自"按名字收组"(登记表里写着 Inner 的名字),前者来自"登记被删/改名/被过滤"。
//   旧行为:Inner 被当成"沦为成员的旧组"保护下来,但它已经【没有任何成员】
//   → 视图上 Outer 底下挂一个零成员的空组行;而且它不在 page.ownedGroups 里
//   → dropProjection/uninstall 碰不到它 → liveGroupCount() 也数不到它
//   → **静默泄漏,连"退出时活组=0"这条断言都抓不到**。
//   ★ 本块的主判据不是"看不到空壳",而是恒等式
//        m_groupOwner 条目数 == liveGroupCount()
//     —— "组归属表"与"活组账本"必须一致;空壳(活着但不归任何页管)会打破它。
//   分辨力:去掉 revalidate 里的 closeMembership 调用 →
//     ① childCount(Outer)==1 变 2 ② 归属表等式变 2==1 —— 两行都会红。
static void t24_shellGroup() {
    hdr("T24", "空壳组(规则自相矛盾:自己的成员没了,却还被别的组收着)",
        "内层组失去成员后必须退役;归属表条目数必须恒等于活组数(空壳会打破该等式)");
    auto* root = new NamedData("R24");
    auto* a = new PartObject("A", "M", root);
    auto* b = new PartObject("B", "M", root);
    auto* c = new PartObject("C", "M", root);
    root->addSubDataGroup(a);
    root->addSubDataGroup(b);
    root->addSubDataGroup(c);

    bool hasInner = true;        // A/B 是否仍给 Inner 出票(关掉 = 内层组失去全部成员)
    ProjectionSpec spec;
    spec.grouping = [&](GuiBase* n) -> std::optional<GroupTicket> {
        if (auto* g = dynamic_cast<TreeGroup*>(n)) {
            // ★ 只收 key=="Inner" 的组,否则 Outer 会把自己也收进去(无限套娃)
            if (g->ticket().key != "Inner") return std::nullopt;
            GroupTicket t; t.key = "Outer"; t.label = "Outer"; return t;
        }
        if (auto* p = dynamic_cast<PartObject*>(n)) {
            if (demoNodeName(p) == "C") {                      // C 一直属于 Outer(外层不会变空)
                GroupTicket t; t.key = "Outer"; t.label = "Outer"; return t;
            }
            if (hasInner) {                                    // A/B 进 Inner(可关掉)
                GroupTicket t; t.key = "Inner"; t.label = "Inner"; return t;
            }
        }
        return std::nullopt;
    };
    spec.opt.dynamicAll = true;
    P.registerRoot(root);

    // 两次 install:第一次只有数据节点 → 只建 Inner;第二次 Inner 作为旧组被收进 Outer
    P.install(root, spec, {});
    P.install(root, spec, {});
    TreeGroup* outer = findGroup(root, "Outer");
    TreeGroup* inner = outer ? dynamic_cast<TreeGroup*>(P.child(outer, 0)) : nullptr;
    showIn("两层: root -> Outer -> Inner -> {A,B};另有直属 C");
    if (!outer || !inner) {
        check(false, "T24 前置:两层嵌套没建起来(显式判失败,不许静默 return)");
        P.uninstall(root); P.unregisterRoot(root); P.collectGarbage(); clearData(root); delete root;
        return;
    }
    // 前置同时是"合法嵌套不被误杀"的基线:活的嵌套组必须原样存活
    check(P.childCount(outer) == 2 && P.childCount(inner) == 2
          && P.parentOf(a) == inner && P.parentOf(c) == outer,
          "T24 前置:Outer=[Inner,C]、Inner=[A,B],坐标逐层正确(活嵌套的基线)");

    const size_t ownerEntries = P.memoryBreakdown().groupOwner;
    showOut("基线:活组=" + std::to_string(P.liveGroupCount())
            + " 归属表条目=" + std::to_string(ownerEntries));
    check(ownerEntries == P.liveGroupCount(),
          "基线:归属表条目数 == 活组数(每页账本与全局归属表一致)");

    // ── 核心:关掉内层规则 → Inner 失去自己的桶,但仍被 Outer 的成员名单"按名字收着"
    hasInner = false;
    P.install(root, spec, {});
    const size_t ownerEntries2 = P.memoryBreakdown().groupOwner;
    showOut("关掉内层规则后: childCount(Outer)=" + std::to_string(P.childCount(outer))
            + " childCount(Inner)=" + std::to_string(P.childCount(inner))
            + " 活组=" + std::to_string(P.liveGroupCount())
            + " 归属表条目=" + std::to_string(ownerEntries2));
    check(P.childCount(outer) == 1,
          "★空壳必须退役:Outer 只剩 C 一个成员(Inner 被留下就是 FAIL)");
    check(P.parentOf(a) == root && P.parentOf(b) == root,
          "★Inner 的成员回落为直属(parentOf=数据父 root)");
    check(ownerEntries2 == P.liveGroupCount(),
          "★归属表条目数 == 活组数(空壳泄漏会打破这个等式 —— 修复前本行必红)");

    // ── 收尾:走调用方该走的那条路(uninstall + 回收)→ 两个账本必须双双归零。
    //   空壳会在这里留下 m_groupOwner 残留(它不在 page.ownedGroups 里,uninstall 遍历不到)
    //   —— 这正是"泄漏检测的盲区":liveGroupCount() 答 0,而归属表还剩 1 条。
    P.uninstall(root);
    P.collectGarbage();
    showOut("uninstall 后: 活组=" + std::to_string(P.liveGroupCount())
            + " 归属表条目=" + std::to_string(P.memoryBreakdown().groupOwner));
    check(P.liveGroupCount() == 0 && P.memoryBreakdown().groupOwner == 0,
          "★uninstall 后:活组与归属表都归零(空壳会留下归属表残留 —— 修复前本行必红)");

    P.unregisterRoot(root);
    P.collectGarbage();
    clearData(root);
    delete root;
}

// T25:ProjectionStages::closeMembership 的纯函数测试(成员关系的闭包)
//   它是 Stage B-2b 的唯一实现,也是"空壳组"缺陷的正面对策 ——
//   按工程约定(每个 ProjectionStages 纯函数配"输入→输出"实例)单列一块。
//   四类边界:空输入 / 已闭合(不误杀) / 单壳(含"宿主只剩空壳") / 多层依赖(必须迭代收敛)。
//   ★ 分辨力:第 ④ 条是给"把迭代改成单轮过滤"准备的红靶子 ——
//     单轮实现会停在 A:[B](dropped=1、桶不退空),该断言立刻变红。
//   ⚠ 纯函数纪律:它【不删】任何组对象,测试自己负责 delete。
static void t25_closeMembership() {
    hdr("T25", "ProjectionStages::closeMembership(成员关系闭包)",
        "剔除空壳组 + 丢掉随之变空的桶;空输入/已闭合/单壳/多层依赖四类边界");
    using ProjectionStages::Bucket;

    NamedData d1("D1"), d2("D2");              // 只当"不是组"的成员用,不接入任何树

    auto makeGroup = [](const std::string& key) {
        GroupTicket t; t.key = key; t.label = key;
        return new TreeGroup(t, nullptr);       // owner=null:本函数只读 ticket().key,不会 deref owner
    };
    auto makeBucket = [](const std::string& key, std::vector<GuiBase*> members) {
        Bucket b;
        b.ticket.key = key;
        b.ticket.label = key;
        b.members = std::move(members);
        return b;
    };

    // ① 空输入
    {
        std::vector<Bucket> buckets;
        const size_t dropped = ProjectionStages::closeMembership(buckets);
        check(dropped == 0 && buckets.empty(), "① 空输入:返回 0、保持空");
    }
    // ② 已闭合:成员全是数据节点 → 一个都不动(不误杀)
    {
        std::vector<Bucket> buckets = { makeBucket("A", { &d1, &d2 }) };
        const size_t dropped = ProjectionStages::closeMembership(buckets);
        check(dropped == 0 && buckets.size() == 1 && buckets[0].members.size() == 2,
              "② 已闭合:没有组成员 → 原样不动(dropped=0)");
    }
    // ③ 单壳:组成员 key=Inner 却没有 Inner 桶 ⇒ 剔除该成员,宿主桶其余成员保留
    {
        TreeGroup* shell = makeGroup("Inner");
        std::vector<Bucket> buckets = { makeBucket("Outer", { shell, &d1 }) };
        const size_t dropped = ProjectionStages::closeMembership(buckets);
        check(dropped == 1 && buckets.size() == 1
              && buckets[0].members.size() == 1 && buckets[0].members[0] == &d1,
              "③ 单壳:空壳组被剔除、宿主桶保留其余成员(dropped=1)");
        delete shell;
    }
    // ③b 宿主桶只剩那个壳 ⇒ 桶变空 ⇒ 连桶一起丢(否则视图留一个空组行)
    {
        TreeGroup* shell = makeGroup("Inner");
        std::vector<Bucket> buckets = { makeBucket("Outer", { shell }) };
        const size_t dropped = ProjectionStages::closeMembership(buckets);
        check(dropped == 1 && buckets.empty(), "③b 宿主只剩空壳:连桶一起丢掉");
        delete shell;
    }
    // ④ 多层依赖:必须迭代到不动点
    //    A:[B]  B:[C]  C 无桶 ⇒ C 是壳 → 剔 → B 空 → 丢桶 ⇒ B 变非代表 → 从 A 剔 → A 空 → 丢桶
    {
        TreeGroup* gA = makeGroup("A");
        TreeGroup* gB = makeGroup("B");
        TreeGroup* gC = makeGroup("C");
        std::vector<Bucket> buckets = { makeBucket("A", { gB }), makeBucket("B", { gC }) };
        const size_t dropped = ProjectionStages::closeMembership(buckets);
        showOut("多层依赖:dropped=" + std::to_string(dropped)
                + " 剩余桶=" + std::to_string(buckets.size()) + "(期望 2 / 0)");
        check(dropped == 2 && buckets.empty(),
              "★④ 多层依赖:C 是壳 → B 随之变壳 → A 也随之变空;迭代两轮收敛(dropped=2、桶全空)");
        delete gA; delete gB; delete gC;    // 纯函数不删对象,测试自删
    }
}

// T26:引擎公开 API 面的系统性覆盖 —— 通知族 / 写侧入口 / 读侧边界 / 回收粒度
//   为什么单列(2026-10-07 引入行覆盖率测量后发现的):下面这些【交付给内网的公开
//   API 与分支】在整个电池里从没被调用过,或只走了主路径、分支一次都没分叉:
//     apply() / refresh() / install(nullptr) / 静态 pool(siblings) /
//     invalidateNode() / invalidateAll() / markGroupPagesDirty 的传递分支 /
//     collectGarbageSince() / healIfDrift 的三条闸门 + 等长内容漂移 /
//     读侧四件套的空指针与"无页父亲"分支 / 宽亲(>256)槽位表 / 塌缩清桶。
//   "没测到就等于没验证" —— 这些是接口面,不是内部实现细节。
static void t26_apiSurface() {
    hdr("T26", "引擎 API 面(通知族/写侧入口/读侧边界/回收粒度)",
        "把电池从没碰过的公开接口与分支逐个走一遍");
    using ProjectionStages::Bucket;

    // ── ① 读侧四件套的边界:空指针 / 越界 / "无页父亲"(透传)──
    {
        auto* root = new NamedData("R26a");
        auto* leaf = new PartObject("L", "M", root);
        root->addSubDataGroup(leaf);
        showIn("root 下 1 个零件,【不装投影】—— 读侧走透传分支");
        check(P.childCount(nullptr) == 0 && P.child(nullptr, 0) == nullptr
              && P.parentOf(nullptr) == nullptr && P.rowOf(nullptr) == -1,
              "① 空指针边界:childCount=0 / child=nullptr / parentOf=nullptr / rowOf=-1");
        check(P.child(root, 99) == nullptr, "① child 越界 → nullptr");
        check(P.parentOf(leaf) == root && P.rowOf(leaf) == 0,
              "① 无页父亲:parentOf 给自然父亲、rowOf 扫原始序列兜底(=0)");
        P.invalidate(root);        // 无页 → 早返回(透传读永远新鲜)
        P.invalidateNode(root);    // 同上
        P.refresh(root);           // 无页 → 早返回
        check(P.rowOf(leaf) == 0, "① 三个通知/刷新入口在'无页'上都是安全的空操作");
        check(P.install(nullptr, ProjectionSpec{}).empty(), "① install(nullptr) 返回空表");
        P.invalidate(nullptr); P.invalidateNode(nullptr); P.notifyRemove(nullptr, nullptr);
        check(true, "① 全部通知入口的空指针守卫不崩");
        P.unregisterRoot(root);
        delete root;
    }

    // ── ② apply / refresh / structRev:一段真实页上的写侧入口 ──
    {
        auto* root = new NamedData("R26b");
        auto* a = new PartObject("A", "Steel", root);
        auto* b = new PartObject("B", "Alum", root);
        auto* c = new PartObject("C", "Steel", root);
        root->addSubDataGroup(a); root->addSubDataGroup(b); root->addSubDataGroup(c);
        P.registerRoot(root);
        auto rule = [](GuiBase* n) -> std::optional<GroupTicket> {
            auto* p = dynamic_cast<PartObject*>(n);
            if (!p) return std::nullopt;
            GroupTicket t; t.key = p->material(); t.label = p->material(); return t;
        };
        auto groups = P.apply(root, { a, b, c }, rule, { false, true });   // ③ apply 入口
        check(groups.size() == 2 && P.hasProjection(root),
              "② apply():按材料建出 2 个组(Steel/Alum)");
        const size_t rev0 = P.structRev();
        P.refresh(root);                                                  // ③ refresh 入口
        check(P.structRev() == rev0, "② refresh() 幂等:重算同一 Spec 不改变形状版本号(structRev 不变)");
        // 换规则 → 形状变 → structRev 必须递增(它是失步自检的判据)
        auto one = [](GuiBase*) -> std::optional<GroupTicket> {
            GroupTicket t; t.key = "All"; t.label = "All"; return t;
        };
        P.apply(root, { a, b, c }, one, { false, true });
        check(P.structRev() > rev0, "② 换规则后 structRev 递增(形状真的变了)");
        P.uninstall(root);
        P.unregisterRoot(root);
        P.collectGarbage();
        delete root;
    }

    // ── ③ 静态参与名单(pool / siblings):dynamicAll=false 的整条路 ──
    {
        auto* root = new NamedData("R26c");
        auto* a = new PartObject("A", "Steel", root);
        auto* b = new PartObject("B", "Steel", root);
        auto* c = new PartObject("C", "Steel", root);
        root->addSubDataGroup(a); root->addSubDataGroup(b); root->addSubDataGroup(c);
        P.registerRoot(root);
        auto rule = [](GuiBase*) -> std::optional<GroupTicket> {
            GroupTicket t; t.key = "G"; t.label = "G"; return t;
        };
        // 名单只给 a、b:规则对 c 连问都不问
        auto g1 = P.install(root, ProjectionSpec{ nullptr, rule, nullptr, { false, false } }, { a, b });
        check(g1.size() == 1 && P.childCount(g1[0]) == 2,
              "③ pool 静态名单:a/b 进组、c 留直属(规则对名单外的节点不出票)");
        check(P.parentOf(c) == root, "③ 名单外的 c 是直属");
        // ★ 名单里塞【本父亲的旧组】→ 必须展开为它的当前活成员(否则换规则时组内成员全体失踪)
        auto g2 = P.install(root, ProjectionSpec{ nullptr, rule, nullptr, { false, false } },
                            { g1[0] });   // 只给旧组,不给 a/b
        check(g2.size() == 1 && P.childCount(g2[0]) == 2,
              "③ siblings 传【旧组】时展开为它的活成员(a/b 没失踪)");
        check(P.parentOf(c) == root, "③ 展开名单不影响名单外的节点");
        P.uninstall(root);
        P.unregisterRoot(root);
        P.collectGarbage();
        delete root;
    }

    // ── ④ 通知族:invalidateNode(含组分支)/ invalidateAll / 传递置脏 ──
    {
        auto* root = new NamedData("R26d");
        auto* folder = new FolderObject("F", root);
        auto* m1 = new PartObject("M1", "Steel", folder);
        auto* m2 = new PartObject("M2", "Alum", folder);
        folder->addSubDataGroup(m1); folder->addSubDataGroup(m2);
        root->addSubDataGroup(folder);
        P.registerRoot(root);
        auto rule = [](GuiBase* n) -> std::optional<GroupTicket> {
            auto* p = dynamic_cast<PartObject*>(n);
            if (!p) return std::nullopt;
            GroupTicket t; t.key = p->material(); t.label = p->material(); return t;
        };
        ProjectionSpec spec{ nullptr, rule, nullptr, { false, true } };
        P.install(folder, spec, {});
        P.install(root, spec, {});
        TreeGroup* steel = findGroup(folder, "Steel");
        check(steel != nullptr, "④ 前置:folder 页建出 Steel 组");
        P.invalidateNode(m1);                       // ← 数据节点分支:经 parentForTree 找自然父
        check(P.parentOf(m1) != nullptr, "④ invalidateNode(数据节点)不崩、投影仍可用");
        P.invalidateNode(steel);                    // ← 组分支:走 owner()
        check(P.parentOf(m1) != nullptr, "④ invalidateNode(组)走 owner() 分支,不崩");
        P.invalidateAll();                          // ← 全量置脏
        check(P.childCount(folder) == 2, "④ invalidateAll() 后逐页收敛,读数正确");
        // 传递置脏:owner 变 → 名下"有自己页的组"连坐(给组也装一页)
        P.install(steel, spec, {});
        P.invalidate(folder);                       // markGroupPagesDirty 要遍历 steel 的页
        check(P.childCount(steel) >= 0, "④ 给组装页后 invalidate(owner) 的传递置脏路径可跑");
        P.uninstall(steel);
        P.uninstall(folder);
        P.uninstall(root);
        P.unregisterRoot(root);
        P.collectGarbage();
        delete root;
    }

    // ── ⑤ 回收粒度:collectGarbageSince 的两段(水位早退 / 只回收 watermark 之后)──
    {
        auto* root = new NamedData("R26e");
        auto* k1 = new PartObject("K1", "M1", root);
        auto* k2 = new PartObject("K2", "M2", root);
        root->addSubDataGroup(k1); root->addSubDataGroup(k2);
        P.registerRoot(root);
        auto rule = [](GuiBase* n) -> std::optional<GroupTicket> {
            auto* p = dynamic_cast<PartObject*>(n);
            if (!p) return std::nullopt;
            GroupTicket t; t.key = p->material(); t.label = p->material(); return t;
        };
        P.install(root, ProjectionSpec{ nullptr, rule, nullptr, { false, true } }, {});
        (void)P.childCount(root);
        check(P.liveGroupCount() == 2, "⑤ 前置:两个组各一个成员");
        check(!P.hasRetired() && P.retiredCount() == 0, "⑤ 前置:队列为空");
        // 静默删掉两个成员(不开括号)→ 纯读触发蒸发 → 两个组进队列。
        // ⚠ 这两个节点挂在 DataObject 政体的 m_subDataGroups 里,必须用
        //   destroySubDataGroup 摘除+删除(removeTreeChild 只管 m_children,摘不掉它们)
        root->destroySubDataGroup(k1);
        root->destroySubDataGroup(k2);
        P.invalidate(root);        // ★ 守约通知,但【不开任何 begin/end 括号】——
                                   //   这正是 P4 探针钉住的那条路径。Release 下若不通知,
                                   //   纯读只会答陈旧缓存(v4 契约),不会发生蒸发。
        (void)P.childCount(root);
        const size_t queued = P.retiredCount();
        showOut("静默删光两个成员后: 活组=" + std::to_string(P.liveGroupCount())
                + " 退役队列=" + std::to_string(queued) + "(期望活组0/队列2)");
        check(queued == 2, "⑤ 两个组都进了退役队列(retiredCount=2)");
        P.collectGarbageSince(queued);              // 水位 ≥ size → 早退
        check(P.retiredCount() == queued, "⑤ collectGarbageSince(水位≥size)早退:一个都不回收");
        P.collectGarbageSince(1);                   // 只回收 [1,·):留 1 个
        check(P.retiredCount() == 1, "⑤ collectGarbageSince(1) 只回收 watermark 之后 → 剩 1");
        P.collectGarbage();                          // 整队列
        check(P.retiredCount() == 0, "⑤ collectGarbage() 清空整队列");
        P.uninstall(root);
        P.unregisterRoot(root);
        P.collectGarbage();
        delete root;
    }

    // ── ⑥ 自愈闸门:healIfDrift 的早退 + "等长内容漂移"的逐元素比对 ──
    {
        auto* root = new NamedData("R26f");
        auto* s1 = new PartObject("S1", "Steel", root);
        auto* a1 = new PartObject("A1", "Alum", root);
        root->addSubDataGroup(s1);
        root->addSubDataGroup(a1);
        P.registerRoot(root);
        auto byMat = [](GuiBase* n) -> std::optional<GroupTicket> {
            auto* p = dynamic_cast<PartObject*>(n);
            if (!p) return std::nullopt;
            GroupTicket t; t.key = p->material(); t.label = p->material(); return t;
        };
        P.install(root, ProjectionSpec{ nullptr, byMat, nullptr, { false, true } }, {});
        check(P.liveGroupCount() == 2, "⑥ 前置:Steel/Alum 两个组");
        const bool oldParanoid = P.paranoidChecks();
        P.setParanoidChecks(true);
        // 闸门②(已知脏页):先通知再读 → healIfDrift 必须在 dirty 处早退(不然会误点名)
        P.invalidate(root);
        (void)P.childCount(root);
        check(P.liveGroupCount() == 2, "⑥ 闸门②:已通知的脏页重算后仍是 2 组(早退且不误报)");
        // ★ 等长内容漂移:静默"删一个 Steel + 加一个 Alum" ⇒ 孩子数不变、内容变了。
        //   只比长度的探测会漏掉它 —— 必须逐元素比对才抓得到(那段循环存在的理由)。
        root->destroySubDataGroup(s1);
        auto* a2 = new PartObject("A2", "Alum", root);
        root->addSubDataGroup(a2);
        // ⚠ 判据要取【数据侧】:投影侧的 childCount 在被读到时已经自愈了(paranoid 是同步的)
        check(root->visibleChildrenCount() == 2,
              "⑥ 等长漂移:数据侧孩子数仍是 2 —— 只比长度的探测抓不到这种漂移");
        // 纯观测不重算(liveGroupCount 只数账本,不触发 revalidate)——所以此刻仍是陈旧的 2 组
        check(P.liveGroupCount() == 2,
              "⑥ 陈旧态:未发生任何读之前,投影仍是 2 组(liveGroupCount 是纯观测,不触发重算)");
        (void)P.childCount(root);      // ← 就是这一次读,触发 healIfDrift 的逐元素比对
        check(P.liveGroupCount() == 1,
              "★⑥ paranoid 逐元素比对发现等长漂移并自愈:Steel 组已蒸发(真的比了内容)");
        P.setParanoidChecks(oldParanoid);
        P.uninstall(root);
        P.unregisterRoot(root);
        P.collectGarbage();
        delete root;
    }

    // ── ⑦ 宽亲(>256):槽位查找切到哈希索引那一档 + 塌缩清桶 ──
    {
        auto* root = new NamedData("R26g");
        const int WIDE = 300;                       // > 256 ⇒ useSlotMap 分支
        std::vector<GuiBase*> kidsW;
        for (int i = 0; i < WIDE; ++i) {
            auto* k = new PartObject("W" + std::to_string(i), (i % 2) ? "A" : "B", root);
            root->addSubDataGroup(k);
            kidsW.push_back(k);
        }
        P.registerRoot(root);
        auto rule = [](GuiBase* n) -> std::optional<GroupTicket> {
            auto* p = dynamic_cast<PartObject*>(n);
            if (!p) return std::nullopt;
            GroupTicket t; t.key = p->material(); t.label = p->material(); return t;
        };
        P.install(root, ProjectionSpec{ nullptr, rule, nullptr, { false, true } }, {});
        // 第二次 install:这时 oldGroups 非空 → 走"按槽位插入 + 宽亲哈希索引"那一档
        P.install(root, ProjectionSpec{ nullptr, rule, nullptr, { false, true } }, {});
        check(P.childCount(root) == 2, "⑦ 宽亲(300 孩子)分组后仍是 2 个组");
        check(P.childCount(findGroup(root, "A")) + P.childCount(findGroup(root, "B")) == WIDE,
              "⑦ 300 个成员一个不少(宽亲槽位表路径正确)");
        // 塌缩清桶:全员同组 + collapseIfSingleGroup ⇒ 不建组(shouldCollapse 的 clear 分支)
        auto all = [](GuiBase*) -> std::optional<GroupTicket> {
            GroupTicket t; t.key = "All"; t.label = "All"; return t;
        };
        P.install(root, ProjectionSpec{ nullptr, all, nullptr, { true /*collapse*/, true } }, {});
        check(P.liveGroupCount() == 0 && P.childCount(root) == WIDE,
              "⑦ collapseIfSingleGroup:全员同组 → 清桶不建废层级(300 行直属)");
        P.uninstall(root);
        P.unregisterRoot(root);
        P.collectGarbage();
        clearData(root);
        delete root;
    }
}

// T27:基础件的【直接】单测 —— PtrSet 全接口 + GuiBase 默认政体 + GeomObject
//   为什么单列:这两个头文件是三政体与 prune DFS 的地基,但电池一直只"间接"用到它们:
//     · PtrSet 只走了 insert 的"首次插入"路径 —— contains/clear/reserve/grow 全没碰过;
//     · GuiBase 的【默认实现】(m_children 政体:addTreeChild/removeTreeChild/
//       visibleChildrenCount/visibleChild/indexInParent/parentForTree)从没被执行过,
//       因为测试用的节点全是 DataObject/DataObject+QObject 派生。
//   地基没测到 = 上层结论的全部前提都没测到。
static void t27_primitives() {
    hdr("T27", "基础件直接单测(PtrSet 全接口 / GuiBase 默认政体 / GeomObject)",
        "三政体与 prune DFS 的地基:键集容器 + 展示接口的默认实现");

    // ── ① PtrSet:insert 去重 / contains / reserve / clear / grow ──
    {
        PtrSet s;
        std::vector<int*> ptrs;
        for (int i = 0; i < 200; ++i) ptrs.push_back(new int(i));   // 造 200 个不同地址
        check(s.insert(ptrs[0]) && !s.insert(ptrs[0]) && s.size() == 1,
              "① insert 返回 true=首次 / false=重复,size 不重复计数");
        check(s.insert(nullptr) == false && s.size() == 1, "① nullptr 永不进表");
        check(s.contains(ptrs[0]) && !s.contains(ptrs[1]), "① contains 命中/未命中");
        check(!s.contains(nullptr), "① contains(nullptr) 恒 false");
        s.reserve(1000);                            // ← reserve 入口
        check(s.size() == 1 && s.contains(ptrs[0]), "① reserve 后内容不丢");
        for (int i = 1; i < 200; ++i) s.insert(ptrs[i]);   // 触发 grow(负载 0.7)
        check(s.size() == 200, "① 插满 200 个(grow 反复扩容后计数仍精确)");
        bool allIn = true;
        for (auto* p : ptrs) allIn = allIn && s.contains(p);
        check(allIn, "① 扩容后 200 个键全部仍可命中(hash & 线性探测在 rehash 后正确)");
        s.clear();                                  // ← clear 入口
        check(s.size() == 0 && !s.contains(ptrs[0]), "① clear 后为空且键不可命中");
        s.insert(ptrs[5]);
        check(s.contains(ptrs[5]), "① clear 只清内容、保留容量 → 之后仍可正常插入");
        for (auto* p : ptrs) delete p;
    }

    // ── ② GuiBase 的默认政体(纯 m_children):展示接口的地基 ──
    {
        struct RawGui : GuiBase {
            explicit RawGui(GuiBase* parent = nullptr) : GuiBase(parent) {}
            std::string type() override { return "Raw"; }
        };
        RawGui root;
        auto* c1 = new RawGui(&root);
        auto* c2 = new RawGui(&root);
        root.addTreeChild(c1);
        root.addTreeChild(c2);
        check(root.visibleChildrenCount() == 2 && root.visibleChild(0) == c1
              && root.visibleChild(1) == c2 && root.type() == "Raw",
              "② GuiBase 默认枚举:m_children 顺序直出");
        check(c1->indexInParent() == 0 && c2->indexInParent() == 1,
              "② indexInParent:按 m_children 线性查找");
        check(c2->parentForTree() == &root && c2->parentForTree()->type() == "Raw",
              "② parentForTree:默认实现读 m_parentForTree");
        root.updateVisibleChilren();   // 默认实现是空体:三政体下由派生类重写
        check(root.visibleChildrenCount() == 2, "② updateVisibleChilren 默认空实现不影响枚举");
        root.removeTreeChild(c1);
        check(root.visibleChildrenCount() == 1 && root.visibleChild(0) == c2
              && c1->indexInParent() == -1,
              "② removeTreeChild 只摘不删;摘掉后 indexInParent 返回 -1");
        delete c1;                 // 所有权在调用方(removeTreeChild 的契约)
        RawGui orphan;
        check(orphan.parentForTree() == nullptr && orphan.indexInParent() == -1,
              "② 无父亲的节点:parentForTree=nullptr、indexInParent=-1");
        // root 析构时连坐删除 c2(不可观测,但这条路径必须被执行到 —— ~GuiBase 的循环)
    }

    // ── ③ 外部消费方才会走的展示接口 + registerRoot 守卫 ──
    //   这些行引擎内部不调(引擎走窄接口),但它们是 GuiBase 契约的一部分 ——
    //   内网若有别的消费方(比如自绘树、导出器)就会用到,所以必须被验证过。
    {
        auto* root = new NamedData("R27c");
        auto* leaf = new PartObject("L", "Metal", root);
        root->addSubDataGroup(leaf);
        P.registerRoot(root);
        auto rule = [](GuiBase* n) -> std::optional<GroupTicket> {
            auto* p = dynamic_cast<PartObject*>(n);
            if (!p) return std::nullopt;
            GroupTicket t; t.key = p->material(); t.label = p->material(); return t;
        };
        P.install(root, ProjectionSpec{ nullptr, rule, nullptr, { false, true } }, {});
        (void)P.childCount(root);
        TreeGroup* g = findGroup(root, "Metal");
        if (!g) {
            check(false, "③ 前置:没建出 Metal 组(中止,不许静默跳过)");
        } else {
            check(g->type() == "TreeGroup", "③ TreeGroup::type() 对外可辨识");
            check(g->parentForTree() == root, "③ TreeGroup::parentForTree() = owner(展示父亲提示位)");
            check(g->visibleChildrenCount() == P.childCount(g) && g->visibleChildrenCount() == 1,
                  "③ TreeGroup::visibleChildrenCount() 委托引擎,与账本一致");
            check(g->visibleChild(0) == P.child(g, 0) && g->visibleChild(0) == leaf,
                  "③ TreeGroup::visibleChild() 同样委托引擎");
        }
        check(root->type() == "Data", "③ DataObject::type() 对外可辨识");
        P.registerRoot(root);          // 重复注册:幂等(内部早返回)
        P.registerRoot(nullptr);       // 空指针守卫
        P.unregisterRoot(nullptr);
        check(P.hasProjection(root), "③ registerRoot 的重复/空指针守卫不崩且不影响投影");
        P.uninstall(root);
        P.unregisterRoot(root);
        P.collectGarbage();
        clearData(root);
        delete root;
    }

    // ── ④ GeomObject:QObject 政体的展示父亲回退 ──
    {
        GeomObject g(nullptr, nullptr);
        check(g.type() == "Geom" && g.parentForTree() == nullptr,
              "③ GeomObject:type=Geom、无父亲时 parentForTree 回退到 QObject 树(这里为空)");
        check(g.visibleChildrenCount() == 0, "③ GeomObject 默认无孩子");
    }
}

int main() {
#ifdef _MSC_VER
    silenceCrtDialogs();          // MSVC CRT 专有(MinGW 覆盖率构建下无此 API)
#endif
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
#ifndef NT_ENGINE_ONLY
    printf("NavTreeGroup unit_tests:25 块,每块打印 职责/输入/输出/校验\n");
#else
    printf("NavTreeGroup unit_tests(engine-only):21 块(T15/T16/T18/T19 需 Qt 模型,已排除)\n");
#endif
    t1_filter();  t2_bucket();  t3_collapse(); t4_reuse(); t5_flat(); t6_sort();
    t7_endToEnd(); t8_cache(); t9_rowOf(); t10_evaporate(); t11_multiLayer(); t12_deathOfOwner();
    t13_prune(); t14_rebind();
#ifndef NT_ENGINE_ONLY
    t15_model(); t16_signals();
    t17_readSideRelease(); t18_modelProtocol(); t19_cascadeMultiLayer();
#else
    t17_readSideRelease();
#endif
    t20_paranoidAndHeal();
    t21_wideReadCost(); t22_bareCoordFreshness(); t23_nodePageCleanup(); t24_shellGroup(); t25_closeMembership();
    t26_apiSurface(); t27_primitives();
    printf("\n══════ 汇总:%s(失败块数 = %d,即退出码)══════\n",
#ifndef NT_ENGINE_ONLY
           g_fail ? "有 FAIL" : "27 块全绿", g_fail);
#else
           g_fail ? "有 FAIL" : "23 块全绿(engine-only)", g_fail);
#endif
    printf("engine live groups at exit = %d\n", (int)P.liveGroupCount());
    return g_fail ? 1 : 0;
}
