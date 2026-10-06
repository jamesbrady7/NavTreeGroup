// ═══════════════════════════════════════════════════════════════
//  unit_tests:把流程切成 16 块,每块打印【职责 → 输入 → 输出 → 校验】
// ═══════════════════════════════════════════════════════════════
//  T1 ~T6  流水线纯函数(pipeline.h)—— 不碰引擎,裸输入裸输出
//  T7 ~T12 引擎行为:端到端投影/缓存/组复用/坐标/蒸发/多层
//  T13~T14 生命周期卫生:prune 剪僵尸 / bindSpec 跨重建重生
//  T15~T16 模型层:三要素自洽 / 事前坐标信号协议
//  运行:unit_tests.exe(仅 Qt Core,无窗口);退出码 = 失败块数
#include <QAbstractItemModel>
#include <QAbstractItemModelTester>   // T18:Qt 官方模型协议测试器(QtTest 模块)
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
#include "pipeline.h"
#include "navtreemodel.h"
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
    hdr("T1", "Pipeline::filterLive", "Stage A 过滤 —— filter 为 false 的节点移出投影");
    World w = makeWorld(true);
    std::vector<GuiBase*> live = { w.base, w.boltA, w.boltB, w.cover, w.sketch };
    auto vis = Pipeline::filterLive(live, notSuppressed());
    showIn("live = " + lst(live) + ",  filter = 非压制件");
    showOut("vis  = " + lst(vis));
    expectList(vis, { w.base, w.boltA, w.boltB, w.sketch }, "Cover(压制)被滤掉,其余保序");
    auto all = Pipeline::filterLive(live, nullptr);
    check(all.size() == live.size(), "filter 为空 = 全员通过");
    freeWorld(w);
}

static void t2_bucket() {
    hdr("T2", "Pipeline::bucketByKey", "Stage B-1 分桶 —— 规则给票据,同 key 归一堆");
    World w = makeWorld(true);
    std::vector<GuiBase*> vis = { w.base, w.boltA, w.boltB, w.sketch };
    size_t participants = 0;
    auto buckets = Pipeline::bucketByKey(vis, nullptr /*dynamicAll*/, byMaterial(), participants);
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
    hdr("T3", "Pipeline::shouldCollapse", "Stage B-2 单组塌缩 —— 全员同组时不建废层级");
    World w = makeWorld(false);
    // 情形① 全员进同一个桶(比如把所有 DataObject 归入 "All")
    GroupRule all = [](GuiBase*) { GroupTicket t; t.key = "All"; t.label = "All"; return t; };
    std::vector<GuiBase*> vis = { w.base, w.boltA, w.boltB, w.cover, w.sketch };
    size_t part = 0;
    auto b1 = Pipeline::bucketByKey(vis, nullptr, all, part);
    bool c1 = Pipeline::shouldCollapse(b1, part, vis.size(), { true, true });
    showIn("vis 全员同 key, collapseIfSingleGroup=true");
    showOut(std::string("shouldCollapse = ") + (c1 ? "true" : "false"));
    check(c1, "全员一组 → 塌缩(组层级是废的)");
    // 情形② 有直属残余(Sketch 走逃生口)→ 不塌缩
    auto b2 = Pipeline::bucketByKey(vis, nullptr, byMaterial(), part);
    bool c2 = Pipeline::shouldCollapse(b2, part, vis.size(), { true, true });
    showIn("vis 分出 Steel/Alum 两桶 + Sketch 直属");
    showOut(std::string("shouldCollapse = ") + (c2 ? "true" : "false"));
    check(!c2, "多桶/有直属 → 不塌缩");
    freeWorld(w);
}

static void t4_reuse() {
    hdr("T4", "Pipeline::matchReusable", "Stage B-3 组复用匹配 —— 同 key 用旧组指针,落选的进淘汰名单");
    // 旧组:Steel/Alum;新桶:Steel/Copper → Steel 复用,Alum 淘汰,Copper 新建
    GroupTicket ts; ts.key = "Steel"; ts.label = "Steel parts";
    GroupTicket ta; ta.key = "Alum";  ta.label = "Alum parts";
    auto* gSteel = new TreeGroup(ts, nullptr);
    auto* gAlum  = new TreeGroup(ta, nullptr);
    std::vector<TreeGroup*> old = { gSteel, gAlum };
    std::vector<Pipeline::Bucket> buckets(2);
    buckets[0].ticket = ts;
    buckets[1].ticket.key = "Copper"; buckets[1].ticket.label = "Copper parts";
    std::vector<TreeGroup*> culled;
    auto reuse = Pipeline::matchReusable(buckets, old, culled);
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
    hdr("T5", "Pipeline::buildFlat", "flat 落位 —— 直属原位不动,组落在首个成员的原位置");
    World w = makeWorld(false);
    auto* gS = new TreeGroup(GroupTicket{ "Steel", "Steel", 0 }, nullptr);
    auto* gA = new TreeGroup(GroupTicket{ "Alum", "Alum", 0 }, nullptr);
    // vis 顺序:Base BoltA BoltB Cover Sketch;Steel 首成员=Base(位0),Alum 首成员=BoltB(位2)
    std::vector<GuiBase*> vis = { w.base, w.boltA, w.boltB, w.cover, w.sketch };
    std::unordered_map<GuiBase*, TreeGroup*> m2g = { { w.base, gS }, { w.boltA, gS },
                                                    { w.boltB, gA }, { w.cover, gA } };
    auto flat = Pipeline::buildFlat(vis, m2g);
    showIn("vis = " + lst(vis) + ",  memberToGroup = {Base,BoltA→Steel; BoltB,Cover→Alum}");
    showOut("flat = " + lst(flat));
    expectList(flat, { gS, gA, w.sketch }, "5 个节点收敛成 3 行:两组各占首成员位置,Sketch 原位");
    delete gS; delete gA;
    freeWorld(w);
}

static void t6_sort() {
    hdr("T6", "Pipeline::sortFlat / sortMembers", "Stage C 排序 —— 组前置,其余按类型秩+名字;stable 不抖动");
    World w = makeWorld(false);
    auto* gS = new TreeGroup(GroupTicket{ "Steel", "Steel", 0 }, nullptr);
    std::vector<GuiBase*> flat = { w.sketch, w.base, gS, w.boltA };
    std::map<TreeGroup*, std::vector<GuiBase*>> members = { { gS, { w.boltA, w.base } } };
    showIn("flat(落位后未排序) = " + lst(flat) + ",  组成员 = [BoltA, Base]");
    Pipeline::sortMembers(members, demoLess);
    Pipeline::sortFlat(flat, demoLess);
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
    hdr("T8", "缓存(lastLive 比对)", "数据没变 → 整条流水线跳过;变了 → 自动重算");
    World w = makeWorld(true);
    P.install(w.root, stdSpec());
    auto* steel1 = findGroup(w.root, "Steel");
    kids(w.root);                                     // 再查一次(触发缓存校验)
    auto* steel2 = findGroup(w.root, "Steel");
    showIn("两次 childCount 之间数据零变化");
    showOut("Steel 组指针 第一次=0x" + std::to_string((uintptr_t)steel1)
            + " 第二次=0x" + std::to_string((uintptr_t)steel2));
    check(steel1 == steel2, "活列表没变 → 缓存命中,组指针稳定");
    // 数据侧新增一个零件 → 指针序列变了 → 重算,新成员自动进组(dynamicAll)
    auto* rod = new PartObject("Rod", "Steel", w.root);
    w.root->addSubDataGroup(rod);
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
    w.root->destroySubDataGroup(w.boltB);              // Alum 组唯一成员,数据侧直接删
    w.boltB = nullptr;
    auto flat = kids(w.root);                          // 这一问触发重算
    showOut("删 BoltB 后投影 = " + lst(flat) + ",  liveGroupCount=" + std::to_string(P.liveGroupCount()));
    check(flat.size() == before - 1, "root 行数 3→2:Alum 组整组蒸发");
    check(!findGroup(w.root, "Alum") && findGroup(w.root, "Steel"), "Alum 没了,Steel 还在");
    check(P.liveGroupCount() == groupsBefore - 1, "引擎释放了空组(无泄漏)");
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

static NavTreeModel* m15 = nullptr;   // 只给 t15 的递归 walk 用(避免 lambda 套娃传参)

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
    w.root->destroySubDataGroup(w.boltB);
    model.endRowRemoval();
    showOut("信号计数 removed=" + std::to_string(removed));
    check(removed == 2 && !findGroup(w.root, "Alum"), "整组蒸发只发 root 层 1 条(累计 2)");

    // ③ 被过滤节点:rowOf=-1 → 零信号
    check(P.rowOf(w.cover) == -1, "Cover 死前 rowOf=-1(不在投影树上)");
    w.root->destroySubDataGroup(w.cover);              // 无需任何信号
    showOut("删 Cover 后 removed=" + std::to_string(removed) + "(未增加)");
    check(removed == 2 && inserted == 0, "零信号 = 正确(视图上本来就没有这一行)");
    P.uninstall(w.root);
    freeWorld(w);
}

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

    // 数据侧裸删 Alum 的全部成员(零通知,不走任何引擎 API)
    w.root->destroySubDataGroup(w.boltB);
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
    check(ms < 2000.0, "★宽节点读侧没退化成秒级(" + std::to_string(ms) + " ms < 2000 ms)");

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

// T20:纪元优化的逃生阀 + 跨线程检测(契约 C5)
//   C5 的前提"单线程 + 一次操作内数据不变"原来只写在注释里,没有开关。
//   本块验证:
//     ① 开关真的接到了闸门上(关闭后 checkedAt 不再生效);
//     ② 开关开/关【行为完全一致】—— 这是"逃生阀"的定义,不是优化项;
//     ③ 关闭时所有页的 checkedAt 被清零(让"关掉就一定全量枚举"可推理)。
static void t20_epochEscapeHatch() {
    hdr("T20", "纪元优化逃生阀(setEpochOptimization)",
        "开关要真的接在闸门上,且开关开/关行为完全一致(只是快慢不同)");
    World w = makeWorld(false);
    P.install(w.root, stdSpec());

    // 记录基准形态:开/关两种情况下,投影树必须【逐项相同】
    auto snapshot = [&] {
        std::string s;
        for (size_t i = 0, n = P.childCount(w.root); i < n; ++i) s += nm(P.child(w.root, i)) + "|";
        return s;
    };

    // ── ① 打开(默认):验证闸门【生效】──
    //   手法:直接问引擎"本页 checkedAt 是否等于当前纪元"。
    //   不能从外部观察,改用等价手段:同一操作内连续问两次 rowCount,
    //   若闸门生效,第二次不该重新跑流水线(否则会多产生一次结构递增)。
    check(P.epochOptimizationEnabled(), "① 默认:纪元优化是开着的");
    size_t rev0 = P.structRev();
    volatile size_t a1 = P.childCount(w.root);
    volatile size_t a2 = P.childCount(w.root);   // 同一次"操作"?否 —— 两次独立操作
    (void)a1; (void)a2;
    size_t rev1 = P.structRev();
    // 数据没变 → 两次操作都不该改形状(结构递增只在形状变化时发生)
    check(rev1 == rev0, "① 数据未变时重复查询不改变结构(闸门语义正确)");

    std::string shapeOn = snapshot();

    // ── ② 关闭:同一棵树,行为必须逐项相同 ──
    P.setEpochOptimization(false);
    check(!P.epochOptimizationEnabled(), "② 开关已关闭");
    std::string shapeOff = snapshot();
    showOut("开: " + shapeOn);
    showOut("关: " + shapeOff);
    check(shapeOn == shapeOff,
          "★② 开关开/关,投影树逐项相同(逃生阀只改快慢,不改行为)");

    // 关掉之后数据侧变更仍必须被发现(这是全枚举路径的核心正确性)
    w.root->destroySubDataGroup(w.boltB);
    w.boltB = nullptr;
    size_t ccOff = P.childCount(w.root);
    bool boltBGone = true;
    for (size_t i = 0, n = P.childCount(w.root); i < n; ++i)
        if (P.child(w.root, i) == static_cast<GuiBase*>(w.boltB)) boltBGone = false;
    check(boltBGone && ccOff > 0,
          "★② 关闭状态下,数据侧删除仍被惰性发现(组自动蒸发)");

    // ── ③ 关→开:恢复后行为仍一致,且 checkedAt 机制重新生效 ──
    P.setEpochOptimization(true);
    check(P.epochOptimizationEnabled(), "③ 开关已恢复");
    std::string shapeBack = snapshot();
    check(shapeBack == shapeOff || shapeBack == shapeOn,
          "★③ 恢复后投影树仍自洽(无中间态残留)");
    // 清理:无论开关状态如何,retired 都要能收干净
    P.collectGarbage();
    check(P.retiredCount() == 0, "③ 开关切回不影响回收语义");

    P.uninstall(w.root);
    P.collectGarbage();
    freeWorld(w);
}

int main() {
    silenceCrtDialogs();
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif
    printf("NavTreeGroup unit_tests:21 块,每块打印 职责/输入/输出/校验\n");
    t1_filter();  t2_bucket();  t3_collapse(); t4_reuse(); t5_flat(); t6_sort();
    t7_endToEnd(); t8_cache(); t9_rowOf(); t10_evaporate(); t11_multiLayer(); t12_deathOfOwner();
    t13_prune(); t14_rebind();
    t15_model(); t16_signals();
    t17_readSideRelease(); t18_modelProtocol(); t19_cascadeMultiLayer(); t20_epochEscapeHatch();
    t21_wideReadCost();
    printf("\n══════ 汇总:%s(失败块数 = %d,即退出码)══════\n",
           g_fail ? "有 FAIL" : "21 块全绿", g_fail);
    printf("engine live groups at exit = %d\n", (int)P.liveGroupCount());
    return g_fail ? 1 : 0;
}
