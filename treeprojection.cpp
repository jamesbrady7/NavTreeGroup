#include "treeprojection.h"
#include "pipeline.h"
#include <algorithm>
#include <cstdint>      // SIZE_MAX
#include <cstdio>       // fprintf(stderr) —— paranoid 漏报点名

// ═══════════════════════════════════════════════════════════════
//  TreeGroup:成员枚举全部委托引擎 → 自身无状态、无指针可悬空
// ═══════════════════════════════════════════════════════════════
size_t TreeGroup::visibleChildrenCount() const {
    return TreeProjection::instance().childCount(const_cast<TreeGroup*>(this));
}
GuiBase* TreeGroup::visibleChild(size_t index) const {
    return TreeProjection::instance().child(const_cast<TreeGroup*>(this), index);
}

// ═══════════════════════════════════════════════════════════════
//  TreeProjection
// ═══════════════════════════════════════════════════════════════
TreeProjection& TreeProjection::instance() {
    static TreeProjection e;
    return e;
}

TreeProjection::~TreeProjection() {
    // 引擎是所有 TreeGroup 的唯一所有者,退出时级联释放(零泄漏)
    std::vector<GuiBase*> keys;
    for (auto& kv : m_byParent) keys.push_back(kv.first);
    for (GuiBase* k : keys) dropProjection(k);
    collectGarbage();   // 兜底:释放还挂在 m_retired 里的(应用忘了调的)
}

bool TreeProjection::isGroup(GuiBase* node) const {
    return dynamic_cast<TreeGroup*>(node) != nullptr;
}

bool TreeProjection::hasProjection(GuiBase* parent) const {
    return m_byParent.find(parent) != m_byParent.end();
}

size_t TreeProjection::liveGroupCount() const {
    size_t n = 0;
    for (auto& kv : m_byParent) n += kv.second.groups.size();
    return n;
}

// 账本内存分解:只读遍历,不改任何状态。
// 字节口径:哈希表 = 桶数组(capacity×8) + 节点(n×24~32 含 malloc 头);
//          std::map = n×48;vector = capacity×8;组成员向量按 capacity 算。
TreeProjection::MemBreakdown TreeProjection::memoryBreakdown() const {
    MemBreakdown mb;
    size_t specPerPage = 0;
    auto hashBytes = [](size_t n, size_t nodeBytes) {
        size_t buckets = 16; while (buckets < (n ? n * 2 : 1)) buckets <<= 1;  // ~0.5 负载
        return buckets * 8 + n * nodeBytes;
    };
    for (const auto& kv : m_byParent) {
        const Projection& p = kv.second;
        ++mb.pages;
        mb.poolNodes += p.pool.size();
        mb.lastLivePtr += p.lastLive.capacity();
        mb.flatPtr += p.flat.capacity();
        mb.memberGroups += p.members.size();
        for (const auto& mk : p.members) mb.memberPtr += mk.second.capacity();
        mb.entries += p.entries.size();          // ★ v4 单表(替代 memberToGroup+memberToPos)
        mb.groupsVec += p.groups.capacity();
        if (specPerPage == 0) {
            specPerPage = 32 * 3 + 32 * 2 + 8 + 16;
            if (p.spec.filter || p.spec.grouping || p.spec.less) specPerPage += 64;
        }
    }
    mb.groupOwner = m_groupOwner.size();
    mb.pageObjBytes = hashBytes(mb.pages, sizeof(void*) * 2 + 8 + 232);
    mb.specBytes = mb.pages * specPerPage;
    mb.lastLiveBytes = mb.lastLivePtr * 8;
    mb.flatBytes = mb.flatPtr * 8;
    mb.membersBytes = mb.memberGroups * 48 + mb.memberPtr * 8 + mb.memberGroups * 16;
    // entries:每槽 = key(8) + PosEntry(16) + 节点头(16)
    mb.entriesBytes = hashBytes(mb.entries, 8 + 16 + 16);
    mb.groupsBytes = mb.groupsVec * 8;
    mb.groupOwnerBytes = hashBytes(mb.groupOwner, 8 + 8 + 8 + 16);
    mb.groupObjBytes = mb.groupOwner * 96;   // TreeGroup 本体估算
    mb.retiredGroups = m_retired.size();
    mb.retiredHighWater = m_retiredHighWater;
    mb.retiredBytes = mb.retiredGroups * 96;
    return mb;
}

// ── 写侧入口 ─────────────────────────────────────────
std::vector<TreeGroup*> TreeProjection::install(GuiBase* parent, const ProjectionSpec& spec,
                                                std::vector<GuiBase*> siblings)
{
    prune();   // 操作边界自动自愈(清僵尸投影 + 漏通知收敛;批量 delete+new 后尤其重要)
    return installInternal(parent, spec, siblings);
}

std::vector<TreeGroup*> TreeProjection::apply(GuiBase* parent, const std::vector<GuiBase*>& siblings,
                                              GroupRule rule, Options opt)
{
    ProjectionSpec spec;
    spec.grouping = std::move(rule);
    spec.opt = opt;
    return install(parent, spec, siblings);
}

std::vector<TreeGroup*> TreeProjection::installInternal(GuiBase* parent, const ProjectionSpec& spec,
                                                        const std::vector<GuiBase*>& siblings)
{
    if (!parent) return {};
    Projection& p = projection(parent);   // unordered_map 节点式:引用在任何插入下稳定
    p.spec = spec;
    // 手动安装:抹掉绑定出身(此页从此按"手动 Spec"对待,rebind 不再豁免它)
    p.bindId.clear();
    p.bindGen = ~0u;

    // 分组参与名单消毒(仅 !dynamicAll 时有意义):
    //   nullptr/parent 自己/重复 → 剔除;"别人家的孩子"不在活枚举里天然无效;
    //   ★ 输入含本父亲的旧组 → 展开为其当前活成员(否则换规则时组内成员全体失踪)
    p.pool.clear();
    if (spec.grouping && !spec.opt.dynamicAll) {
        for (GuiBase* n : siblings) {
            if (!n || n == parent) continue;
            if (auto* g = dynamic_cast<TreeGroup*>(n)) {
                auto oit = m_groupOwner.find(g);
                if (oit != m_groupOwner.end() && oit->second == parent) {
                    auto mit = p.members.find(g);
                    if (mit != p.members.end())
                        for (GuiBase* m : mit->second) p.pool.insert(m);
                }
                continue;   // 外来组 → 忽略
            }
            p.pool.insert(n);
        }
    }

    p.valid = false;          // 强制重算
    p.dirty = true;
    revalidate(parent, p);
    return p.groups;
}

// ══ ★数据侧变更通知(v4 契约)══════════════════════════════
//  通知 = 置脏标志,O(1),不触发重算(批量变更天然合并到下次读)。

void TreeProjection::invalidate(GuiBase* parent) {
    if (!parent) return;
    auto it = m_byParent.find(parent);
    if (it == m_byParent.end()) return;   // 无页 = 没人投影它的孩子列表(透传读永远现场新鲜)
    it->second.dirty = true;
    markGroupPagesDirty(parent);          // 组成员表源自本页 → 名下组页连坐置脏(传递)
}

void TreeProjection::invalidateNode(GuiBase* node) {
    if (!node) return;
    // 节点属性的消费方是它【投影父亲】所在的页 = 自然父亲的页(组页由传递置脏连坐)。
    // 自然父亲没页 = 没有页按它的属性分组/过滤/排序它 → 无事发生。
    GuiBase* p = dynamic_cast<TreeGroup*>(node)
               ? static_cast<TreeGroup*>(node)->owner()
               : node->parentForTree();
    invalidate(p);
}

void TreeProjection::notifyRemove(GuiBase* parent, GuiBase* dying) {
    // 顺序无强约束,但都必须在 delete dying 之前:
    invalidate(parent);   // 孩子列表脏(下次读重算,死者自然缺席)
    discard(dying);       // dying 自己的账页(若有)当场拆掉;其内部页变僵尸由 prune 清
}

void TreeProjection::invalidateAll() {
    for (auto& kv : m_byParent) kv.second.dirty = true;
}

// 把 parent 名下(传递)所有"有自己账页"的组置脏。
// 为什么必须传递:组页的活列表 = owner 成员表;owner 变 → 组页输入变。
// 若只在 owner 重算时传播(revalidate 末尾也有一份),会出现
// "owner 已置脏但还没人读,先读组页 → 组页干净 → 答陈旧 flat"的窗口。
void TreeProjection::markGroupPagesDirty(GuiBase* parent) {
    auto it = m_byParent.find(parent);
    if (it == m_byParent.end()) return;
    PtrSet seen;
    std::vector<GuiBase*> stack;
    for (TreeGroup* g : it->second.groups) stack.push_back(g);
    while (!stack.empty()) {
        GuiBase* g = stack.back(); stack.pop_back();
        if (!g || !seen.insert(g)) continue;
        auto git = m_byParent.find(g);
        if (git == m_byParent.end()) continue;
        git->second.dirty = true;
        for (TreeGroup* gg : git->second.groups) stack.push_back(gg);
    }
}

// ── rawChildren:不带本节点投影的"原始孩子"枚举 ──────
// ★ 多层分组的关键函数:
//   · TreeGroup 的原始孩子 = 外层 owner 投影解析出的当前成员(递归终止于数据侧枚举);
//   · 普通节点 = 数据侧虚接口现场重算(死者天然缺席)。
// revalidate 内部只准用它取活列表:若走公共 childCount,
// "对组分组"时 内层revalidate → childCount(组) → 又触发内层revalidate → 无限递归。
std::vector<GuiBase*> TreeProjection::rawChildren(GuiBase* node) {
    std::vector<GuiBase*> out;
    if (!node) return out;
    if (TreeGroup* g = dynamic_cast<TreeGroup*>(node)) {
        if (const auto* mem = freshMembers(g)) out = *mem;
        return out;
    }
    size_t n = node->visibleChildrenCount();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(node->visibleChild(i));
    return out;
}

// 组的"新鲜成员表"就地引用:owner 页先 revalidate,然后直接返回账本里的向量地址 —— 不拷贝。
// ⚠ 指针有效期到 owner 页下次重算为止;所有调用点都是"取得即用"。
// 无 owner / owner 无页 / 组不在成员表(已被淘汰)→ nullptr(等价旧行为:空表)。
const std::vector<GuiBase*>* TreeProjection::freshMembers(TreeGroup* g) {
    auto oit = m_groupOwner.find(g);
    if (oit == m_groupOwner.end()) return nullptr;
    auto pit = m_byParent.find(oit->second);
    if (pit == m_byParent.end()) return nullptr;
    revalidate(oit->second, pit->second);          // 先保证外层成员表新鲜
    // ★ revalidate 可能整页重算(members 被 move 重建)→ 必须回查迭代器,不得复用旧的。
    pit = m_byParent.find(oit->second);
    if (pit == m_byParent.end()) return nullptr;
    auto mit = pit->second.members.find(g);
    return mit == pit->second.members.end() ? nullptr : &mit->second;
}

// ── 自愈:枚举原始孩子与 lastLive 比对,漂移 = 调用方漏了通知 ──
// 使用点:① prune 的 DFS(卫生边界,Release 也跑,verbose=paranoid);
//         ② childCount 的 paranoid 检测(默认 Debug,verbose=true 当场点名)。
// 发现漂移:置脏(本次/下次 revalidate 收敛)+ 可选 stderr 报告。绝不 delete 任何东西。
bool TreeProjection::healIfDrift(GuiBase* node, Projection& p, bool verbose) {
    if (!p.valid) return false;                    // 没算过的页无所谓陈旧
    if (p.dirty) return false;                     // ★已被通知的脏页:漂移是【预期内】的,
                                                   //   马上会被 revalidate 重算,不是漏报
                                                   //   (没有这行,notifyRemove→读 的正常流程会被误点名)
    const std::vector<GuiBase*>* mem = nullptr;
    std::vector<GuiBase*> rawBuf;
    if (auto* g = dynamic_cast<TreeGroup*>(node)) {
        mem = freshMembers(g);                     // 触发 owner 重算 —— 可能连锁把本组淘汰!
        if (m_byParent.find(node) == m_byParent.end()) return false;   // 本页已被拆:无需自愈
    } else {
        size_t n = node->visibleChildrenCount();
        rawBuf.reserve(n);
        for (size_t i = 0; i < n; ++i) rawBuf.push_back(node->visibleChild(i));
        mem = &rawBuf;
    }
    size_t n = mem ? mem->size() : 0;
    bool drift = (n != p.lastLive.size());
    if (!drift)
        for (size_t i = 0; i < n; ++i)
            if ((*mem)[i] != p.lastLive[i]) { drift = true; break; }
    if (drift) {
        p.dirty = true;
        if (verbose)
            fprintf(stderr,
                    "[TreeProjection] 漏报检测:owner=%p 的数据已变化但未收到 invalidate —— 已自愈。\n"
                    "  请在这个父亲的数据变更后调用 invalidate(parent)(删除用 notifyRemove)。\n",
                    (void*)node);
    }
    return drift;
}

// ── revalidate:引擎心脏 = 流水线的【编排器】────────────
// v4 缓存检查只有一行:干净页零枚举直接返回;脏页重算一次。
// 各阶段是 pipeline.h 里的纯函数(可独立单测,见 tests.cpp T1~T6);
// 这里只剩带所有权/引擎状态的事:建组、淘汰组(延迟回收)、写账本、传播置脏。
void TreeProjection::revalidate(GuiBase* parent, Projection& p) {
    // 0. 缓存检查的全部(v3 的纪元闸门/就地比对/双路径全部退役):
    if (p.valid && !p.dirty) return;

    // 组页前置:本页活列表 = owner 成员表,要靠 owner 重算解析;owner 可能恰好
    // 把这组淘汰(成员全灭)并顺手拆掉本页 —— p 当场悬空。先取表、确认本页还在账上。
    if (auto* selfG = dynamic_cast<TreeGroup*>(parent)) {
        (void)freshMembers(selfG);                          // 触发 owner 重算
        if (m_byParent.find(parent) == m_byParent.end()) return;   // 本页已被拆:放弃
    }

    p.valid = false;   // ★ 重算开始即撤销"可呈现"承诺:流水线中用户回调(filter/grouping/less)
                       //   若抛异常,本页停在 valid=false + dirty=true → 下次查询整条重跑收敛,
                       //   绝不会短路返回含野指针的旧 flat(异常安全闸门,契约 §1.5)。

    // 1. 拉活快照(swap 进账本,免一次大向量拷贝);lastLive 从此兼职"自愈参照"
    std::vector<GuiBase*> live = rawChildren(parent);
    p.lastLive.swap(live);

    // Stage A:过滤(被滤节点移出投影,rowOf=-1;"搜索定位掀开过滤器"是上层职责)
    std::vector<GuiBase*> vis = Pipeline::filterLive(p.lastLive, p.spec.filter);

    // 旧组全单:顶层组 + 成员表里所有层的组(嵌套组也要能被复用/淘汰,否则孤悬泄漏)
    std::vector<TreeGroup*> oldAll = p.groups;
    {
        std::set<TreeGroup*> seenG(oldAll.begin(), oldAll.end());
        for (const auto& kv : p.members)
            if (seenG.insert(kv.first).second) oldAll.push_back(kv.first);
    }

    // 参与名单 parts = vis + 【旧组】(每个旧组插在其"首个 vis 可见后代"的原槽位)——
    // 多层分组的关键:旧组可以作为整体被新规则"再分进"一个外层组。
    // ★ 槽位必须【传递式】下钻:直接成员全是组的组,只看一层会拿不到槽位
    //   → 组从 parts 失踪 → 外层规则看不见它(用户最简复现钉死为 interactive P 步)。
    std::vector<GuiBase*> parts;
    if (oldAll.empty()) {
        parts.swap(vis);          // 冷启动最常见:参与名单就是可见名单
    } else {
        // ⚠ 变量名绝不能叫 slots/signals/emit —— Qt 关键字宏会静默吃掉声明。
        // vis 槽位查找两档自适应:窄亲(≤256)线性扫(连续内存比哈希快);
        // 宽亲(>256)哈希索引(线性会退化成 O(组×成员×孩子))。
        const bool useSlotMap = vis.size() > 256;
        std::unordered_map<GuiBase*, size_t> visSlot;
        if (useSlotMap) {
            visSlot.reserve(vis.size());
            for (size_t i = 0; i < vis.size(); ++i) visSlot.emplace(vis[i], i);
        }
        std::function<size_t(GuiBase*, std::set<GuiBase*>&)> firstVis;
        firstVis = [&](GuiBase* x, std::set<GuiBase*>& guard) -> size_t {
            if (!guard.insert(x).second) return SIZE_MAX;      // 防脏数据成环
            if (useSlotMap) {
                auto sit = visSlot.find(x);
                if (sit != visSlot.end()) return sit->second;  // 数据节点:自己的槽位
            } else {
                for (size_t i = 0; i < vis.size(); ++i)
                    if (vis[i] == x) return i;
            }
            auto it = p.members.find(static_cast<TreeGroup*>(x));
            if (it == p.members.end()) return SIZE_MAX;
            size_t best = SIZE_MAX;                            // 组:取成员里最小的 vis 槽位
            for (GuiBase* m : it->second) {
                size_t r = firstVis(m, guard);
                if (r < best) best = r;
            }
            return best;
        };
        std::vector<std::pair<size_t, TreeGroup*>> groupSlots;
        for (TreeGroup* g : oldAll) {
            std::set<GuiBase*> guard;
            size_t first = firstVis(g, guard);
            if (first != SIZE_MAX) {
                std::pair<size_t, TreeGroup*> slot(first, g);   // MSVC 不吃嵌套花括号,拆具名
                groupSlots.push_back(slot);
            }
        }
        std::stable_sort(groupSlots.begin(), groupSlots.end(),
                         [](const std::pair<size_t, TreeGroup*>& a,
                            const std::pair<size_t, TreeGroup*>& b) { return a.first < b.first; });
        size_t si = 0;
        for (size_t i = 0; i < vis.size(); ++i) {
            while (si < groupSlots.size() && groupSlots[si].first == i)
                parts.push_back(groupSlots[si++].second);
            parts.push_back(vis[i]);
        }
        while (si < groupSlots.size()) parts.push_back(groupSlots[si++].second);   // 兜底
    }

    // Stage B-1/B-2:分桶 + 单组塌缩判定
    size_t participants = 0;
    auto buckets = Pipeline::bucketByKey(parts,
                                         p.spec.opt.dynamicAll ? nullptr : &p.pool,
                                         p.spec.grouping, participants);
    if (Pipeline::shouldCollapse(buckets, participants, parts.size(), p.spec.opt))
        buckets.clear();

    // Stage B-3:复用匹配(纯函数),然后引擎做所有权动作:
    // 复用旧组(指针稳定 → QModelIndex/展开/选中状态跨重算存活)或新建;
    // 未复用的旧组淘汰(★级联拆其内层投影 + 延迟回收,读侧触发绝不 delete —— 契约 C1)
    std::vector<TreeGroup*> culled;
    auto reuse = Pipeline::matchReusable(buckets, oldAll, culled);
    std::map<TreeGroup*, std::vector<GuiBase*>> newMembers;
    for (size_t i = 0; i < buckets.size(); ++i) {
        TreeGroup* g = reuse[i] ? reuse[i] : new TreeGroup(buckets[i].ticket, parent);
        g->updateTicket(buckets[i].ticket);   // label/order 以新 Spec 为准
        newMembers[g] = buckets[i].members;
        m_groupOwner[g] = parent;
    }
    // ★ 沦为"成员"的旧组不能淘汰 —— 它活在外层组里面(多层分组的存活路径)。
    //   仅在有淘汰候选时才全扫成员(dynamic_cast 不便宜,稳态 culled 为空 → 整遍跳过)
    std::set<TreeGroup*> asMember;
    if (!culled.empty())
        for (const auto& kv : newMembers)
            for (GuiBase* m : kv.second)
                if (auto* mg = dynamic_cast<TreeGroup*>(m)) asMember.insert(mg);
    std::set<GuiBase*> deleted;                 // 本轮真淘汰的旧组(落位时必须排除)
    for (TreeGroup* g : culled) {
        if (asMember.count(g)) continue;
        m_groupOwner.erase(g);
        // deferDelete=true:revalidate 由【读侧】(childCount==rowCount)也能触发,
        // 这里绝不能 delete —— 组进 m_retired 等 collectGarbage(契约 C1/C3)。
        dropProjection(g, /*deferDelete=*/true);   // 拆 g 名下的内层投影页
        // ★ g 本身必须单独入队:dropProjection 遍历的是"g 的 p.groups",
        //   g 自己不在其中(它在【本页】的账上)—— 少了这行 = 组既没回收也没记账。
        noteRetired(g);
        deleted.insert(g);
    }

    // Stage B-4:组内成员排序(与全局同一比较器)
    Pipeline::sortMembers(newMembers, p.spec.less);

    // 重建本页成员账本
    p.members = std::move(newMembers);

    // ★ entries 阶段①:成员行(container=所属组,row=组内序号)。
    //   buildFlat 的 rep 上溯依赖这张表,必须先于落位建好。
    p.entries.clear();
    {
        size_t total = 0;
        for (const auto& kv : p.members) total += kv.second.size();
        p.entries.reserve(total + p.lastLive.size());   // 连同 flat 行一次留够,免 rehash
    }
    for (const auto& kv : p.members)
        for (size_t i = 0; i < kv.second.size(); ++i) {
            PosEntry e{ kv.first, (int)i };             // MSVC 嵌套花括号稳妥写法
            p.entries[kv.second[i]] = e;
        }

    // 落位 + Stage C:组落在首成员原位置,然后整体 stable_sort
    // (落位遍历 parts:未进桶的旧组也要作为直属组留在本层;
    //  ★ 本轮已淘汰的旧组必须剔除 —— 它的成员还活着所以仍在 parts 里,
    //    不排除就是把野指针推进 flat,S2 的 use-after-free 就这么来的)
    std::vector<GuiBase*> flatParts;
    flatParts.reserve(parts.size());
    for (GuiBase* x : parts)
        if (!deleted.count(x)) flatParts.push_back(x);
    const size_t oldFlatSize = p.flat.size();
    const size_t oldGroupCount = p.groups.size();
    p.flat = Pipeline::buildFlat(flatParts, p.entries, parent);
    Pipeline::sortFlat(p.flat, p.spec.less);

    // 组账本 = 本层顶层组(flat 里的组)∪ 成员表全部组键(含"作为成员"的嵌套组)。
    // 只记顶层会漏掉嵌套组 → 下轮 matchReusable 认不出它们 → 重算即孤悬。
    {
        std::set<TreeGroup*> seenG;
        std::vector<TreeGroup*> all;
        for (GuiBase* x : p.flat)
            if (auto* gg = dynamic_cast<TreeGroup*>(x))
                if (seenG.insert(gg).second) all.push_back(gg);
        for (const auto& kv : p.members)
            if (seenG.insert(kv.first).second) all.push_back(kv.first);
        p.groups = all;
    }

    // ★ entries 阶段②:flat 行(container=owner,row=flat 序号)。
    //   与阶段①键集零交集:成员不进 flat,flat 里的组/直属不是本页任何组的成员。
    for (size_t i = 0; i < p.flat.size(); ++i) {
        PosEntry e{ parent, (int)i };
        p.entries[p.flat[i]] = e;
    }

    // ★ 保守传播:本页名下凡有自己账页的组一律置脏(成员表可能变了)。
    //   深度=组嵌套层数、成本=O(组数) 的置位;过收敛的代价只是组页多一次重算 ——
    //   用"绝不精确 diff"换"绝不漏传播"(invalidate 侧还有一份传递置脏,双保险)。
    for (TreeGroup* g : p.groups) {
        auto git = m_byParent.find(g);
        if (git != m_byParent.end()) git->second.dirty = true;
    }

    // ★ 流水线全部跑完,现在才宣布有效(异常安全闸门,见 p.valid 注释)
    p.valid = true;
    p.dirty = false;
    // 结构版本号:只有"形状真的变了"才递增(组增删 或 行数变化)。
    // 内容变化(同形状下的成员替换)不递增,因为它不破坏模型坐标。
    if (!deleted.empty() || p.flat.size() != oldFlatSize || p.groups.size() != oldGroupCount)
        ++m_structRev;
}

// ── uninstall:级联删组清账,恢复透传 ──────────────────
// deferDelete:false = 立即 delete(写侧边界,调用方已保证视图坐标失效);
//              true  = 只摘账、组进 m_retired 等 collectGarbage(读侧路径专用,契约 C1)。
void TreeProjection::dropProjection(GuiBase* parent, bool deferDelete) {
    auto it = m_byParent.find(parent);
    if (it == m_byParent.end()) return;
    Projection& p = it->second;
    // ★ v4:位置索引(entries)是页内成员,随 m_byParent.erase 一起析构 ——
    //   没有全局副本要清,v3 R2 那类"登记/清理不对称"泄漏在结构上不可能发生。
    for (TreeGroup* g : p.groups) {
        m_groupOwner.erase(g);
        dropProjection(g, deferDelete);                 // ★级联:组名下的内层投影页先拆
        if (deferDelete) noteRetired(g); else delete g;
    }
    m_byParent.erase(it);   // 哈希表:递归只 erase 别的 key,本迭代器仍有效
}

// 真正释放已淘汰的组 —— 全工程唯一 delete TreeGroup 的地方(除引擎析构与写侧立即释放)。
// ★ 调用时机契约:视图的 persistent index 全部失效之后,即 begin/endResetModel 或
//   begin/endRemoveRows 之内。NavTreeModel 的 endFullReset/endRowRemoval 已自动调用。
void TreeProjection::collectGarbage() {
    for (TreeGroup* g : m_retired) delete g;
    m_retired.clear();
}

void TreeProjection::uninstall(GuiBase* parent) {
    dropProjection(parent);
}

// ══ 生命周期卫生(三政体生存法则) ══

// 腿①:显式讣告。纯 map 操作,全程不解引用 dying(它可能正在析构栈上,虚表已退化)
void TreeProjection::discard(GuiBase* dying) {
    dropProjection(dying);
}

void TreeProjection::registerRoot(GuiBase* root) {
    if (!root) return;
    for (GuiBase* r : m_roots) if (r == root) return;
    m_roots.push_back(root);
}

void TreeProjection::unregisterRoot(GuiBase* root) {
    m_roots.erase(std::remove(m_roots.begin(), m_roots.end(), root), m_roots.end());
}

// prune 的 DFS:只沿活枚举下行,死者走不到也绝不 deref。
// ★ 显式栈迭代(递归版在 10 万级深链上实测爆栈);孩子枚举就地走虚接口/成员表引用。
// ★ v4 新增职责:每到一个账页先 healIfDrift —— prune 是卫生边界,
//   漏通知的陈旧页在这里全部收敛(Release 也跑,这是"忘了通知最多陈旧到下一次 prune"的兑现点)。
void TreeProjection::collectReachable(GuiBase* root, PtrSet& seen) {
    std::vector<GuiBase*> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        GuiBase* n = stack.back();
        stack.pop_back();
        if (!n || !seen.insert(n)) continue;   // 兼防脏数据成环
        auto pit = m_byParent.find(n);
        if (pit != m_byParent.end()) {
            healIfDrift(n, pit->second, /*verbose=*/m_paranoid);   // 漏通知自愈
            pit = m_byParent.find(n);            // heal 的 freshMembers 可能连锁拆组页 → 回查
            if (pit == m_byParent.end()) continue;
            revalidate(n, pit->second);          // 保证 flat 新鲜再走账本
            pit = m_byParent.find(n);            // 组页可能被连锁拆页 → 回查
            if (pit == m_byParent.end()) continue;
            for (GuiBase* c : pit->second.flat) stack.push_back(c);
        } else if (auto* g = dynamic_cast<TreeGroup*>(n)) {
            if (const auto* mem = freshMembers(g))
                for (GuiBase* c : *mem) stack.push_back(c);
        } else {
            size_t cnt = n->visibleChildrenCount();
            for (size_t i = 0; i < cnt; ++i) stack.push_back(n->visibleChild(i));
        }
    }
}

// 腿②:可达性剪枝。走不到的 key = 死者 → 清条目 + 删孤儿组
void TreeProjection::prune() {
    if (m_roots.empty()) return;
    // 账本空 = 没有任何可被剪枝的对象 → 整个 DFS 是纯白走(冷启动首装省一次全树 DFS)
    if (m_byParent.empty() && m_groupOwner.empty()) return;
    PtrSet seen;   // 只做指针值哈希,永不 deref(生存法则不变)
    for (GuiBase* r : m_roots) collectReachable(r, seen);
    std::vector<GuiBase*> stale;
    for (auto& kv : m_byParent)
        if (!seen.contains(kv.first)) stale.push_back(kv.first);
    for (GuiBase* s : stale)
        dropProjection(s);   // 对缺失 key 幂等(级联可能已拆掉)
}

// ══ 腿③:声明式绑定(跨全量重建的投影意图) ══

void TreeProjection::bindSpec(SpecBinding b) {
    if (!b.locate) return;
    unbindSpec(b.id);                       // 同 id 覆盖(幂等;内含 ++m_bindGen)
    m_bindings.push_back(std::move(b));
    ++m_bindGen;                            // 绑定表变了 → 所有"同绑定豁免"作废
    rebind();                               // 注册即生效
}

void TreeProjection::unbindSpec(const std::string& id) {
    m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(),
        [&](const SpecBinding& b) { return b.id == id; }), m_bindings.end());
    ++m_bindGen;                            // 表变动 → 豁免作废(防旧 id 撞新绑定)
    // 注:解绑不拆已装投影(保留现场);下次重建后不再重生。要立即拆用 uninstall。
}

// walkBindings 的安装入口:同绑定豁免。
// 页的 (bindId,bindGen) 与当前绑定一致 → Spec 必然一字未动 → 什么都不做
// (页脏不脏由通知/prune 自愈决定,这里不再强制重算 —— v3 的 walk 因此从 1.1s 降到 0.5s)。
void TreeProjection::installFromBinding(GuiBase* n, const SpecBinding& b) {
    auto pit = m_byParent.find(n);
    if (pit != m_byParent.end() && pit->second.bindGen == m_bindGen
        && pit->second.bindId == b.id)
        return;                             // 同绑定:页已在,数据变更走通知/自愈通道
    installInternal(n, b.spec, {});
    pit = m_byParent.find(n);               // 盖上出身章(下次 rebind 豁免用)
    if (pit != m_byParent.end()) {
        pit->second.bindId = b.id;
        pit->second.bindGen = m_bindGen;
    }
}

// rebind 的 DFS:显式栈迭代,语义与旧递归版逐节点一致
void TreeProjection::walkBindings(GuiBase* root, PtrSet& seen) {
    std::vector<GuiBase*> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        GuiBase* n = stack.back();
        stack.pop_back();
        if (!n || !seen.insert(n)) continue;
        // ★ 绑定谓词【不对组节点求值】(不变量#3):先判非组再跑 locate ——
        //   组 label 与 locate 特征同名时会对组装投影 → 无限嵌套(实测崩在 operator new);
        //   且用户 locate 对组做无防御 cast 即 UB(评审 R3:求值顺序也是契约)。
        if (!isGroup(n))
            for (auto& b : m_bindings)
                if (b.locate(n))
                    installFromBinding(n, b);
        auto pit = m_byParent.find(n);
        if (pit != m_byParent.end()) {
            revalidate(n, pit->second);
            pit = m_byParent.find(n);                 // 组页可能被连锁拆页 → 回查
            if (pit == m_byParent.end()) continue;
            for (GuiBase* c : pit->second.flat) stack.push_back(c);
        } else if (auto* g = dynamic_cast<TreeGroup*>(n)) {
            if (const auto* mem = freshMembers(g))
                for (GuiBase* c : *mem) stack.push_back(c);
        } else {
            size_t cnt = n->visibleChildrenCount();
            for (size_t i = 0; i < cnt; ++i) stack.push_back(n->visibleChild(i));
        }
    }
}

// ★ 全量重建后的标准动作(beginFullReset → wipe → prune → rebuild → rebind → endFullReset)
void TreeProjection::rebind() {
    prune();   // 内含全量自愈:重建后的地址复用幽灵/漏通知陈旧页全部在这里收敛
    if (m_bindings.empty() || m_roots.empty()) return;
    PtrSet seen;
    for (GuiBase* r : m_roots) walkBindings(r, seen);
}

void TreeProjection::refresh(GuiBase* parent) {
    prune();
    auto it = m_byParent.find(parent);
    if (it == m_byParent.end()) return;
    it->second.dirty = true;
    revalidate(parent, it->second);
}

// ══ 读侧四件套(ITreeSource)══

size_t TreeProjection::childCount(GuiBase* node) {
    if (!node) return 0;
    auto it = m_byParent.find(node);
    if (it != m_byParent.end()) {
        if (m_paranoid) {
            // 漏报检测(默认 Debug):干净页也枚举比对一次,漂移即点名+自愈
            healIfDrift(node, it->second, /*verbose=*/true);
            it = m_byParent.find(node);          // heal 可能连锁拆组页 → 回查
            if (it == m_byParent.end()) return 0;
        }
        revalidate(node, it->second);            // 脏才重算;干净页零枚举
        it = m_byParent.find(node);              // 组页可能被连锁拆页 → 回查
        return it == m_byParent.end() ? 0 : it->second.flat.size();
    }
    // 没装页 → 原始枚举。组 = owner 成员表就地取 size;普通节点 = 虚接口直读。
    if (auto* g = dynamic_cast<TreeGroup*>(node)) {
        const auto* mem = freshMembers(g);
        return mem ? mem->size() : 0;
    }
    return node->visibleChildrenCount();
}

GuiBase* TreeProjection::child(GuiBase* node, size_t i) {
    // 约定:模型先 childCount(触发重算)再 child(读缓存)
    if (!node) return nullptr;
    auto it = m_byParent.find(node);
    if (it != m_byParent.end())
        return i < it->second.flat.size() ? it->second.flat[i] : nullptr;
    if (auto* g = dynamic_cast<TreeGroup*>(node)) {
        const auto* mem = freshMembers(g);     // O(1) 就地取,不拷成员表
        return (mem && i < mem->size()) ? (*mem)[i] : nullptr;
    }
    return i < node->visibleChildrenCount() ? node->visibleChild(i) : nullptr;
}

// ── locate:parentOf/rowOf 共用的下钻定位(v4 单表版)──────
// 返回 {投影父亲, 行号};行号 -1 = 需要调用方兜底扫描(透传父)或不在投影树(被过滤)。
// 下钻规则(与 v3 parentOf 逐项等价,但查 entries 单表、干净页零枚举):
//   自然父亲(组=owner,数据节点=parentForTree)起,每层:
//     无页 → 组:行号=上一层记下的成员行;透传数据父:row=-1 交 rowOf 扫原始序列
//     有页 → revalidate 后查 entries[node]:
//            查无 = 本页没投影它(被过滤)→ {p,-1}
//            container==p = 直属 flat 行 → {p,row}
//            container==组 → 记住(组,成员行),下钻进组继续(组可能有自己的页,deepest-wins)
std::pair<GuiBase*, int> TreeProjection::locate(GuiBase* node) {
    if (!node) return { nullptr, -1 };
    GuiBase* p = dynamic_cast<TreeGroup*>(node)
               ? static_cast<TreeGroup*>(node)->owner()   // 组的"自然父亲"=owner
               : node->parentForTree();                   // 既有虚接口(含 Geom 的 QObject 回退)
    GuiBase* lastContainer = nullptr;   // 上一层页给出的 container(=当前 p 时,lastRow 即答案行号)
    int lastRow = -1;
    while (p) {
        auto pit = m_byParent.find(p);
        if (pit == m_byParent.end()) {
            // p 无页:① p 是组 → 成员序列就是孩子序列,行号=上一层页记下的成员行;
            //         ② p 是透传数据父 → row=-1,rowOf 兜底扫原始序列(parentOf 不需要行号,免扫)
            if (lastContainer == p) return { p, lastRow };
            return { p, -1 };
        }
        revalidate(p, pit->second);
        pit = m_byParent.find(p);              // 组页可能被连锁拆页 → 回查
        if (pit == m_byParent.end()) return { p, -1 };   // 页没了(组已淘汰):按"无页"处理,rowOf 兜底得 -1
        auto eit = pit->second.entries.find(node);
        if (eit == pit->second.entries.end()) return { p, -1 };   // 本页没投影它(被过滤/已缺席)
        lastContainer = eit->second.container;
        lastRow = eit->second.row;
        if (eit->second.container == p) return { p, lastRow };    // 直属:flat 行
        p = eit->second.container;                                // 进组,继续下钻
    }
    return { nullptr, -1 };
}

GuiBase* TreeProjection::parentOf(GuiBase* node) {
    if (!node) return nullptr;
    return locate(node).first;
}

int TreeProjection::rowOf(GuiBase* node) {
    if (!node) return -1;
    auto loc = locate(node);
    if (!loc.first) return -1;
    if (loc.second >= 0) return loc.second;    // entries 直接给出(flat 行 或 组成员行)
    // row=-1 的两种情形:
    if (m_byParent.count(loc.first)) return -1;   // ① 有页但 entries 查无 = 被过滤 → 不在投影树
    // ② 透传父亲(无页):就地扫原始序列
    size_t n = loc.first->visibleChildrenCount();
    for (size_t i = 0; i < n; ++i)
        if (loc.first->visibleChild(i) == node) return (int)i;
    return -1;
}
