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
    static TreeProjection engine;
    return engine;
}

TreeProjection::~TreeProjection() {
    // 引擎是所有 TreeGroup 的唯一所有者,退出时级联释放(零泄漏)
    std::vector<GuiBase*> pageOwners;
    for (const auto& pageEntry : m_byParent) {
        pageOwners.push_back(pageEntry.first);
    }
    for (GuiBase* owner : pageOwners) {
        dropProjection(owner);
    }
    collectGarbage();   // 兜底:释放还挂在 m_retired 里的(应用忘了调的)
}

bool TreeProjection::isGroup(GuiBase* node) const {
    return dynamic_cast<TreeGroup*>(node) != nullptr;
}

bool TreeProjection::hasProjection(GuiBase* parent) const {
    return m_byParent.find(parent) != m_byParent.end();
}

size_t TreeProjection::liveGroupCount() const {
    size_t total = 0;
    for (const auto& pageEntry : m_byParent) {
        total += pageEntry.second.groups.size();
    }
    return total;
}

// 账本内存分解:只读遍历,不改任何状态。
// 字节口径:哈希表 = 桶数组(capacity×8) + 节点(n×24~32 含 malloc 头);
//          std::map = n×48;vector = capacity×8;组成员向量按 capacity 算。
TreeProjection::MemBreakdown TreeProjection::memoryBreakdown() const {
    MemBreakdown breakdown;
    size_t specPerPage = 0;
    auto hashBytes = [](size_t entryCount, size_t bytesPerEntry) {
        size_t bucketCount = 16;
        while (bucketCount < (entryCount ? entryCount * 2 : 1)) {
            bucketCount <<= 1;                                  // ~0.5 负载
        }
        return bucketCount * 8 + entryCount * bytesPerEntry;
    };
    for (const auto& pageEntry : m_byParent) {
        const Projection& page = pageEntry.second;
        ++breakdown.pages;
        breakdown.poolNodes += page.pool.size();
        breakdown.lastLivePtr += page.lastLive.capacity();
        breakdown.flatPtr += page.flat.capacity();
        breakdown.memberGroups += page.members.size();
        for (const auto& memberEntry : page.members) {
            breakdown.memberPtr += memberEntry.second.capacity();
        }
        breakdown.entries += page.entries.size();  // ★ v4 单表(替代 memberToGroup+memberToPos)
        breakdown.groupsVec += page.groups.capacity();
        if (specPerPage == 0) {
            specPerPage = 32 * 3 + 32 * 2 + 8 + 16;
            if (page.spec.filter || page.spec.grouping || page.spec.less) {
                specPerPage += 64;
            }
        }
    }
    breakdown.groupOwner = m_groupOwner.size();
    breakdown.pageObjBytes = hashBytes(breakdown.pages, sizeof(void*) * 2 + 8 + 232);
    breakdown.specBytes = breakdown.pages * specPerPage;
    breakdown.lastLiveBytes = breakdown.lastLivePtr * 8;
    breakdown.flatBytes = breakdown.flatPtr * 8;
    breakdown.membersBytes = breakdown.memberGroups * 48 + breakdown.memberPtr * 8 + breakdown.memberGroups * 16;
    // entries:每槽 = key(8) + PosEntry(16) + 节点头(16)
    breakdown.entriesBytes = hashBytes(breakdown.entries, 8 + 16 + 16);
    breakdown.groupsBytes = breakdown.groupsVec * 8;
    breakdown.groupOwnerBytes = hashBytes(breakdown.groupOwner, 8 + 8 + 8 + 16);
    breakdown.groupObjBytes = breakdown.groupOwner * 96;   // TreeGroup 本体估算
    breakdown.retiredGroups = m_retired.size();
    breakdown.retiredHighWater = m_retiredHighWater;
    breakdown.retiredBytes = breakdown.retiredGroups * 96;
    return breakdown;
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
    if (!parent) {
        return {};
    }
    Projection& page = projection(parent);   // unordered_map 节点式:引用在任何插入下稳定
    page.spec = spec;
    // 手动安装:抹掉绑定出身(此页从此按"手动 Spec"对待,rebind 不再豁免它)
    page.bindId.clear();
    page.bindGen = ~0u;

    // 分组参与名单消毒(仅 !dynamicAll 时有意义):
    //   nullptr/parent 自己/重复 → 剔除;"别人家的孩子"不在活枚举里天然无效;
    //   ★ 输入含本父亲的旧组 → 展开为其当前活成员(否则换规则时组内成员全体失踪)
    page.pool.clear();
    if (spec.grouping && !spec.opt.dynamicAll) {
        for (GuiBase* sibling : siblings) {
            if (!sibling || sibling == parent) {
                continue;
            }
            if (auto* siblingGroup = dynamic_cast<TreeGroup*>(sibling)) {
                auto ownerIt = m_groupOwner.find(siblingGroup);
                if (ownerIt != m_groupOwner.end() && ownerIt->second == parent) {
                    auto membersIt = page.members.find(siblingGroup);
                    if (membersIt != page.members.end()) {
                        for (GuiBase* member : membersIt->second) {
                            page.pool.insert(member);
                        }
                    }
                }
                continue;   // 外来组 → 忽略
            }
            page.pool.insert(sibling);
        }
    }

    page.valid = false;          // 强制重算
    page.dirty = true;
    revalidate(parent, page);
    return page.groups;
}

// ══ ★数据侧变更通知(v4 契约)══════════════════════════════
//  通知 = 置脏标志,O(1),不触发重算(批量变更天然合并到下次读)。

void TreeProjection::invalidate(GuiBase* parent) {
    if (!parent) {
        return;
    }
    auto pageIt = m_byParent.find(parent);
    if (pageIt == m_byParent.end()) {
        return;   // 无页 = 没人投影它的孩子列表(透传读永远现场新鲜)
    }
    pageIt->second.dirty = true;
    markGroupPagesDirty(parent);          // 组成员表源自本页 → 名下组页连坐置脏(传递)
}

void TreeProjection::invalidateNode(GuiBase* node) {
    if (!node) {
        return;
    }
    // 节点属性的消费方是它【投影父亲】所在的页 = 自然父亲的页(组页由传递置脏连坐)。
    // 自然父亲没页 = 没有页按它的属性分组/过滤/排序它 → 无事发生。
    GuiBase* naturalParent = dynamic_cast<TreeGroup*>(node)
                           ? static_cast<TreeGroup*>(node)->owner()
                           : node->parentForTree();
    invalidate(naturalParent);
}

void TreeProjection::notifyRemove(GuiBase* parent, GuiBase* dying) {
    // 顺序无强约束,但都必须在 delete dying 之前:
    invalidate(parent);   // 孩子列表脏(下次读重算,死者自然缺席)
    discard(dying);       // dying 自己的账页(若有)当场拆掉;其内部页变僵尸由 prune 清
}

void TreeProjection::invalidateAll() {
    for (auto& pageEntry : m_byParent) {
        pageEntry.second.dirty = true;
    }
}

// 把 parent 名下(传递)所有"有自己账页"的组置脏。
// 为什么必须传递:组页的活列表 = owner 成员表;owner 变 → 组页输入变。
// 若只在 owner 重算时传播(revalidate 末尾也有一份),会出现
// "owner 已置脏但还没人读,先读组页 → 组页干净 → 答陈旧 flat"的窗口。
void TreeProjection::markGroupPagesDirty(GuiBase* parent) {
    auto parentPageIt = m_byParent.find(parent);
    if (parentPageIt == m_byParent.end()) {
        return;
    }
    PtrSet visited;
    std::vector<GuiBase*> pendingGroups;
    for (TreeGroup* group : parentPageIt->second.groups) {
        pendingGroups.push_back(group);
    }
    while (!pendingGroups.empty()) {
        GuiBase* group = pendingGroups.back();
        pendingGroups.pop_back();
        if (!group || !visited.insert(group)) {
            continue;
        }
        auto groupPageIt = m_byParent.find(group);
        if (groupPageIt == m_byParent.end()) {
            continue;
        }
        groupPageIt->second.dirty = true;
        for (TreeGroup* nestedGroup : groupPageIt->second.groups) {
            pendingGroups.push_back(nestedGroup);
        }
    }
}

// ── rawChildren:不带本节点投影的"原始孩子"枚举 ──────
// ★ 多层分组的关键函数:
//   · TreeGroup 的原始孩子 = 外层 owner 投影解析出的当前成员(递归终止于数据侧枚举);
//   · 普通节点 = 数据侧虚接口现场重算(死者天然缺席)。
// revalidate 内部只准用它取活列表:若走公共 childCount,
// "对组分组"时 内层revalidate → childCount(组) → 又触发内层revalidate → 无限递归。
std::vector<GuiBase*> TreeProjection::rawChildren(GuiBase* node) {
    std::vector<GuiBase*> children;
    if (!node) {
        return children;
    }
    if (TreeGroup* group = dynamic_cast<TreeGroup*>(node)) {
        if (const auto* memberList = freshMembers(group)) {
            children = *memberList;
        }
        return children;
    }
    size_t rawCount = node->visibleChildrenCount();
    children.reserve(rawCount);
    for (size_t i = 0; i < rawCount; ++i) {
        children.push_back(node->visibleChild(i));
    }
    return children;
}

// 组的"新鲜成员表"就地引用:owner 页先 revalidate,然后直接返回账本里的向量地址 —— 不拷贝。
// ⚠ 指针有效期到 owner 页下次重算为止;所有调用点都是"取得即用"。
// 无 owner / owner 无页 / 组不在成员表(已被淘汰)→ nullptr(等价旧行为:空表)。
const std::vector<GuiBase*>* TreeProjection::freshMembers(TreeGroup* group) {
    auto ownerIt = m_groupOwner.find(group);
    if (ownerIt == m_groupOwner.end()) {
        return nullptr;
    }
    auto ownerPageIt = m_byParent.find(ownerIt->second);
    if (ownerPageIt == m_byParent.end()) {
        return nullptr;
    }
    revalidate(ownerIt->second, ownerPageIt->second);   // 先保证外层成员表新鲜
    // ★ revalidate 可能整页重算(members 被 move 重建)→ 必须回查迭代器,不得复用旧的。
    ownerPageIt = m_byParent.find(ownerIt->second);
    if (ownerPageIt == m_byParent.end()) {
        return nullptr;
    }
    auto membersIt = ownerPageIt->second.members.find(group);
    return membersIt == ownerPageIt->second.members.end() ? nullptr : &membersIt->second;
}

// ── 自愈:枚举原始孩子与 lastLive 比对,漂移 = 调用方漏了通知 ──
// 使用点:① prune 的 DFS(卫生边界,Release 也跑,verbose=paranoid);
//         ② childCount 的 paranoid 检测(默认 Debug,verbose=true 当场点名)。
// 发现漂移:置脏(本次/下次 revalidate 收敛)+ 可选 stderr 报告。绝不 delete 任何东西。
bool TreeProjection::healIfDrift(GuiBase* node, Projection& page, bool verbose) {
    if (!page.valid) {
        return false;                    // 没算过的页无所谓陈旧
    }
    if (page.dirty) {
        return false;                    // ★已被通知的脏页:漂移是【预期内】的,
                                         //   马上会被 revalidate 重算,不是漏报
                                         //   (没有这行,notifyRemove→读 的正常流程会被误点名)
    }
    const std::vector<GuiBase*>* currentChildren = nullptr;
    std::vector<GuiBase*> enumerated;
    if (auto* group = dynamic_cast<TreeGroup*>(node)) {
        currentChildren = freshMembers(group);   // 触发 owner 重算 —— 可能连锁把本组淘汰!
        if (m_byParent.find(node) == m_byParent.end()) {
            return false;                        // 本页已被拆:无需自愈
        }
    } else {
        size_t rawCount = node->visibleChildrenCount();
        enumerated.reserve(rawCount);
        for (size_t i = 0; i < rawCount; ++i) {
            enumerated.push_back(node->visibleChild(i));
        }
        currentChildren = &enumerated;
    }
    size_t liveCount = currentChildren ? currentChildren->size() : 0;
    bool drifted = (liveCount != page.lastLive.size());
    if (!drifted) {
        for (size_t i = 0; i < liveCount; ++i) {
            if ((*currentChildren)[i] != page.lastLive[i]) {
                drifted = true;
                break;
            }
        }
    }
    if (drifted) {
        page.dirty = true;
        if (verbose) {
            fprintf(stderr,
                    "[TreeProjection] 漏报检测:owner=%p 的数据已变化但未收到 invalidate —— 已自愈。\n"
                    "  请在这个父亲的数据变更后调用 invalidate(parent)(删除用 notifyRemove)。\n",
                    (void*)node);
        }
    }
    return drifted;
}

// ── revalidate:引擎心脏 = 流水线的【编排器】────────────
// v4 缓存检查只有一行:干净页零枚举直接返回;脏页重算一次。
// 各阶段是 pipeline.h 里的纯函数(可独立单测,见 tests.cpp T1~T6);
// 这里只剩带所有权/引擎状态的事:建组、淘汰组(延迟回收)、写账本、传播置脏。
void TreeProjection::revalidate(GuiBase* parent, Projection& page) {
    // 0. 缓存检查的全部(v3 的纪元闸门/就地比对/双路径全部退役):
    if (page.valid && !page.dirty) {
        return;
    }

    // 组页前置:本页活列表 = owner 成员表,要靠 owner 重算解析;owner 可能恰好
    // 把这组淘汰(成员全灭)并顺手拆掉本页 —— page 当场悬空。先取表、确认本页还在账上。
    if (auto* selfGroup = dynamic_cast<TreeGroup*>(parent)) {
        (void)freshMembers(selfGroup);                      // 触发 owner 重算
        if (m_byParent.find(parent) == m_byParent.end()) {
            return;   // 本页已被拆:放弃
        }
    }

    page.valid = false;   // ★ 重算开始即撤销"可呈现"承诺:流水线中用户回调(filter/grouping/less)
                          //   若抛异常,本页停在 valid=false + dirty=true → 下次查询整条重跑收敛,
                          //   绝不会短路返回含野指针的旧 flat(异常安全闸门,契约 §1.5)。

    // 1. 拉活快照(swap 进账本,免一次大向量拷贝);lastLive 从此兼职"自愈参照"
    std::vector<GuiBase*> liveChildren = rawChildren(parent);
    page.lastLive.swap(liveChildren);

    // Stage A:过滤(被滤节点移出投影,rowOf=-1;"搜索定位掀开过滤器"是上层职责)
    std::vector<GuiBase*> visibleNodes = Pipeline::filterLive(page.lastLive, page.spec.filter);

    // 旧组全单:顶层组 + 成员表里所有层的组(嵌套组也要能被复用/淘汰,否则孤悬泄漏)
    std::vector<TreeGroup*> oldGroups = page.groups;
    {
        std::set<TreeGroup*> seenGroups(oldGroups.begin(), oldGroups.end());
        for (const auto& memberEntry : page.members) {
            if (seenGroups.insert(memberEntry.first).second) {
                oldGroups.push_back(memberEntry.first);
            }
        }
    }

    // 参与名单 parts = visibleNodes + 【旧组】(每个旧组插在其"首个 vis 可见后代"的原槽位)——
    // 多层分组的关键:旧组可以作为整体被新规则"再分进"一个外层组。
    // ★ 槽位必须【传递式】下钻:直接成员全是组的组,只看一层会拿不到槽位
    //   → 组从 parts 失踪 → 外层规则看不见它(用户最简复现钉死为 interactive P 步)。
    std::vector<GuiBase*> parts;
    if (oldGroups.empty()) {
        parts.swap(visibleNodes);          // 冷启动最常见:参与名单就是可见名单
    } else {
        // ⚠ 变量名绝不能叫 slots/signals/emit —— Qt 关键字宏会静默吃掉声明。
        // vis 槽位查找两档自适应:窄亲(≤256)线性扫(连续内存比哈希快);
        // 宽亲(>256)哈希索引(线性会退化成 O(组×成员×孩子))。
        const bool useSlotMap = visibleNodes.size() > 256;
        std::unordered_map<GuiBase*, size_t> visibleSlotOf;
        if (useSlotMap) {
            visibleSlotOf.reserve(visibleNodes.size());
            for (size_t i = 0; i < visibleNodes.size(); ++i) {
                visibleSlotOf.emplace(visibleNodes[i], i);
            }
        }
        std::function<size_t(GuiBase*, std::set<GuiBase*>&)> firstVis;
        firstVis = [&](GuiBase* candidate, std::set<GuiBase*>& cycleGuard) -> size_t {
            if (!cycleGuard.insert(candidate).second) {
                return SIZE_MAX;      // 防脏数据成环
            }
            if (useSlotMap) {
                auto slotIt = visibleSlotOf.find(candidate);
                if (slotIt != visibleSlotOf.end()) {
                    return slotIt->second;              // 数据节点:自己的槽位
                }
            } else {
                for (size_t i = 0; i < visibleNodes.size(); ++i) {
                    if (visibleNodes[i] == candidate) {
                        return i;
                    }
                }
            }
            /** 如果是非原始数据（分组节点），走以下逻辑 **/
            auto membersIt = page.members.find(static_cast<TreeGroup*>(candidate));
            if (membersIt == page.members.end()) {
                return SIZE_MAX;
            }
            size_t bestSlot = SIZE_MAX;                 // 组:取成员里最小的 vis 槽位
            for (GuiBase* member : membersIt->second) {
                size_t memberSlot = firstVis(member, cycleGuard);
                if (memberSlot < bestSlot) {
                    bestSlot = memberSlot;
                }
            }
            return bestSlot;
        };
        std::vector<std::pair<size_t, TreeGroup*>> groupSlots;
        for (TreeGroup* oldGroup : oldGroups) {
            std::set<GuiBase*> cycleGuard;
            size_t firstSlot = firstVis(oldGroup, cycleGuard);
            if (firstSlot != SIZE_MAX) {
                std::pair<size_t, TreeGroup*> groupSlot(firstSlot, oldGroup);   // MSVC 不吃嵌套花括号,拆具名
                groupSlots.push_back(groupSlot);
            }
        }
        std::stable_sort(groupSlots.begin(), groupSlots.end(),
                         [](const std::pair<size_t, TreeGroup*>& slotA,
                            const std::pair<size_t, TreeGroup*>& slotB) { return slotA.first < slotB.first; });
        size_t slotCursor = 0;
        for (size_t i = 0; i < visibleNodes.size(); ++i) {
            while (slotCursor < groupSlots.size() && groupSlots[slotCursor].first == i) {
                parts.push_back(groupSlots[slotCursor++].second);
            }
            parts.push_back(visibleNodes[i]);
        }
        while (slotCursor < groupSlots.size()) {
            parts.push_back(groupSlots[slotCursor++].second);   // 兜底
        }
    }

    // Stage B-1/B-2:分桶 + 单组塌缩判定
    size_t participants = 0;
    auto buckets = Pipeline::bucketByKey(parts,
                                         page.spec.opt.dynamicAll ? nullptr : &page.pool,
                                         page.spec.grouping, participants);
    if (Pipeline::shouldCollapse(buckets, participants, parts.size(), page.spec.opt)) {
        buckets.clear();
    }

    // Stage B-2b:成员关系闭包 —— 剔除"自己的 key 没有桶"的空壳组(纯函数,见 pipeline.h)。
    // 不收敛的后果不只是视图多一个空组行:空壳不在 page.groups 里 → 本函数的淘汰分支
    // 遍历不到它、liveGroupCount() 也数不到它 → 只在 m_groupOwner 里留一条静默泄漏。
    if (size_t shellCount = Pipeline::closeMembership(buckets); shellCount != 0) {
#ifdef _DEBUG
        fprintf(stderr,
                "[TreeProjection] 规则输出自相矛盾:丢弃了 %zu 个空壳组"
                "(自己已无成员,却被别的组按名字收着)—— 请检查分组规则/手工登记表。\n",
                shellCount);
#endif
    }

    // Stage B-3:复用匹配(纯函数),然后引擎做所有权动作:
    // 复用旧组(指针稳定 → QModelIndex/展开/选中状态跨重算存活)或新建;
    // 未复用的旧组淘汰(★级联拆其内层投影 + 延迟回收,读侧触发绝不 delete —— 契约 C1)
    std::vector<TreeGroup*> culledGroups;
    auto reusedGroups = Pipeline::matchReusable(buckets, oldGroups, culledGroups);
    std::map<TreeGroup*, std::vector<GuiBase*>> newMembers;
    for (size_t bucketIdx = 0; bucketIdx < buckets.size(); ++bucketIdx) {
        TreeGroup* bucketGroup = reusedGroups[bucketIdx]
                               ? reusedGroups[bucketIdx]
                               : new TreeGroup(buckets[bucketIdx].ticket, parent);
        bucketGroup->updateTicket(buckets[bucketIdx].ticket);   // label/order 以新 Spec 为准
        newMembers[bucketGroup] = buckets[bucketIdx].members;
        m_groupOwner[bucketGroup] = parent;
    }
    // ★ 沦为"成员"的旧组【绝不能】直接淘汰:它的指针已经写进 newMembers[收养者] 的成员表,
    //   而模型会顺着 child(收养者, i) 读到它 —— 回收它 = 让另一条账本持有已释放指针(UAF)。
    //   ⚠ Stage B-2b 的闭包成立后,这个交集【理论上为空】(桶里的组必有桶 ⇒ 必被 matchReusable
    //     认领 ⇒ 不在 culledGroups)。保留下来的唯一理由是那条"同 key 重复旧组"的病态路径:
    //     matchReusable 的 byKey 后者覆盖前者,落选的那个会被报成 culled ——
    //     此时这道白名单把"空壳"降级为"安全但无用地活着",而不是 UAF。
    //     代价:白名单为了不 UAF 必须让它继续存在于成员表里 —— 也就是保留一个空组行。
    //     (若哪天能证明同页 key 唯一,这段可以删除,那时 closeMembership 就是完整答案。)
    //   仅在有淘汰候选时才全扫成员(dynamic_cast 不便宜,稳态 culledGroups 为空 → 整遍跳过)
    std::set<TreeGroup*> groupsKeptAsMember;
    if (!culledGroups.empty()) {
        for (const auto& memberEntry : newMembers) {
            for (GuiBase* member : memberEntry.second) {
                if (auto* memberAsGroup = dynamic_cast<TreeGroup*>(member)) {
                    groupsKeptAsMember.insert(memberAsGroup);
                }
            }
        }
    }
    std::set<GuiBase*> removedGroups;         // 本轮真淘汰的旧组(落位时必须排除)
    for (TreeGroup* culledGroup : culledGroups) {
        if (groupsKeptAsMember.count(culledGroup)) {
            continue;
        }
        m_groupOwner.erase(culledGroup);
        // deferDelete=true:revalidate 由【读侧】(childCount==rowCount)也能触发,
        // 这里绝不能 delete —— 组进 m_retired 等 collectGarbage(契约 C1/C3)。
        dropProjection(culledGroup, /*deferDelete=*/true);   // 拆它名下的内层投影页
        // ★ 组本身必须【单独】入队:dropProjection 遍历的是"该组自己页的 page.groups",
        //   它自己不在其中(它在【本页】的账上)—— 少了这一行 = 组既没回收、也没记账。
        noteRetired(culledGroup);
        removedGroups.insert(culledGroup);
    }

    // Stage B-4:组内成员排序(与全局同一比较器)
    Pipeline::sortMembers(newMembers, page.spec.less);

    // 重建本页成员账本
    page.members = std::move(newMembers);

    // ★ entries 阶段①:成员行(container=所属组,row=组内序号)。
    //   buildFlat 的 rep 上溯依赖这张表,必须先于落位建好。
    page.entries.clear();
    {
        size_t totalMembers = 0;
        for (const auto& memberEntry : page.members) {
            totalMembers += memberEntry.second.size();
        }
        page.entries.reserve(totalMembers + page.lastLive.size());   // 连同 flat 行一次留够,免 rehash
    }
    for (const auto& memberEntry : page.members) {
        for (size_t i = 0; i < memberEntry.second.size(); ++i) {
            PosEntry memberPos{ memberEntry.first, (int)i };         // MSVC 嵌套花括号稳妥写法
            page.entries[memberEntry.second[i]] = memberPos;
        }
    }

    // 落位 + Stage C:组落在首成员原位置,然后整体 stable_sort
    // (落位遍历 parts:未进桶的旧组也要作为直属组留在本层;
    //  ★ 但本轮已淘汰的旧组必须剔除 —— 它的成员还活着所以仍在 parts 里,
    //    不排除就是把野指针推进 flat,S2 的 use-after-free 就这么来的)
    std::vector<GuiBase*> flatParts;
    flatParts.reserve(parts.size());
    for (GuiBase* candidate : parts) {
        if (!removedGroups.count(candidate)) {
            flatParts.push_back(candidate);
        }
    }
    const size_t oldFlatSize = page.flat.size();
    const size_t oldGroupCount = page.groups.size();
    page.flat = Pipeline::buildFlat(flatParts, page.entries, parent);
    Pipeline::sortFlat(page.flat, page.spec.less);

    // 组账本 = 本层顶层组(flat 里的组)∪ 成员表全部组键(含"作为成员"的嵌套组)。
    // 只记顶层会漏掉嵌套组 → 下轮 matchReusable 认不出它们 → 重算即孤悬。
    {
        std::set<TreeGroup*> seenGroups;
        std::vector<TreeGroup*> allGroups;
        for (GuiBase* flatItem : page.flat) {
            if (auto* flatGroup = dynamic_cast<TreeGroup*>(flatItem)) {
                if (seenGroups.insert(flatGroup).second) {
                    allGroups.push_back(flatGroup);
                }
            }
        }
        for (const auto& memberEntry : page.members) {
            if (seenGroups.insert(memberEntry.first).second) {
                allGroups.push_back(memberEntry.first);
            }
        }
        page.groups = allGroups;
    }

    // ★ entries 阶段②:flat 行(container=owner,row=flat 序号)。
    //   与阶段①键集零交集:成员不进 flat,flat 里的组/直属不是本页任何组的成员。
    for (size_t i = 0; i < page.flat.size(); ++i) {
        PosEntry flatPos{ parent, (int)i };
        page.entries[page.flat[i]] = flatPos;
    }

    // ★ 保守传播:本页名下凡有自己账页的组一律置脏(成员表可能变了)。
    //   深度=组嵌套层数、成本=O(组数) 的置位;过收敛的代价只是组页多一次重算 ——
    //   用"绝不精确 diff"换"绝不漏传播"(invalidate 侧还有一份传递置脏,双保险)。
    for (TreeGroup* group : page.groups) {
        auto groupPageIt = m_byParent.find(group);
        if (groupPageIt != m_byParent.end()) {
            groupPageIt->second.dirty = true;
        }
    }

    // ★ 流水线全部跑完,现在才宣布有效(异常安全闸门,见 page.valid 注释)
    page.valid = true;
    page.dirty = false;
    // 结构版本号:只有"形状真的变了"才递增(组增删 或 行数变化)。
    // 内容变化(同形状下的成员替换)不递增,因为它不破坏模型坐标。
    if (!removedGroups.empty() || page.flat.size() != oldFlatSize || page.groups.size() != oldGroupCount) {
        ++m_structRev;
    }
}

// ── uninstall:级联删组清账,恢复透传 ──────────────────
// deferDelete:false = 立即 delete(写侧边界,调用方已保证视图坐标失效);
//              true  = 只摘账、组进 m_retired 等 collectGarbage(读侧路径专用,契约 C1)。
void TreeProjection::dropProjection(GuiBase* parent, bool deferDelete) {
    auto pageIt = m_byParent.find(parent);
    if (pageIt == m_byParent.end()) {
        return;
    }
    Projection& page = pageIt->second;
    // ★ v4:位置索引(entries)是页内成员,随 m_byParent.erase 一起析构 ——
    //   没有全局副本要清,v3 R2 那类"登记/清理不对称"泄漏在结构上不可能发生。
    for (TreeGroup* group : page.groups) {
        m_groupOwner.erase(group);
        dropProjection(group, deferDelete);               // ★级联:组名下的内层投影页先拆
        if (deferDelete) {
            noteRetired(group);
        } else {
            delete group;
        }
    }
    m_byParent.erase(pageIt);   // 哈希表:递归只 erase 别的 key,本迭代器仍有效
}

// 真正释放已淘汰的组 —— 全工程唯一 delete TreeGroup 的地方(除引擎析构与写侧立即释放)。
// ★ 调用时机契约:视图的 persistent index 全部失效之后,即 begin/endResetModel 或
//   begin/endRemoveRows 之内。NavTreeModel 的 endFullReset/endRowRemoval 已自动调用。
void TreeProjection::collectGarbage() {
    for (TreeGroup* retiredGroup : m_retired) {
        delete retiredGroup;
    }
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
    if (!root) {
        return;
    }
    for (GuiBase* registered : m_roots) {
        if (registered == root) {
            return;
        }
    }
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
    std::vector<GuiBase*> dfsStack;
    dfsStack.push_back(root);
    while (!dfsStack.empty()) {
        GuiBase* node = dfsStack.back();
        dfsStack.pop_back();
        if (!node || !seen.insert(node)) {
            continue;   // 兼防脏数据成环
        }
        auto pageIt = m_byParent.find(node);
        if (pageIt != m_byParent.end()) {
            healIfDrift(node, pageIt->second, /*verbose=*/m_paranoid);   // 漏通知自愈
            pageIt = m_byParent.find(node);            // heal 的 freshMembers 可能连锁拆组页 → 回查
            if (pageIt == m_byParent.end()) {
                continue;
            }
            revalidate(node, pageIt->second);          // 保证 flat 新鲜再走账本
            pageIt = m_byParent.find(node);            // 组页可能被连锁拆页 → 回查
            if (pageIt == m_byParent.end()) {
                continue;
            }
            for (GuiBase* flatChild : pageIt->second.flat) {
                dfsStack.push_back(flatChild);
            }
        } else if (auto* group = dynamic_cast<TreeGroup*>(node)) {
            if (const auto* memberList = freshMembers(group)) {
                for (GuiBase* member : *memberList) {
                    dfsStack.push_back(member);
                }
            }
        } else {
            size_t rawCount = node->visibleChildrenCount();
            for (size_t i = 0; i < rawCount; ++i) {
                dfsStack.push_back(node->visibleChild(i));
            }
        }
    }
}

// 腿②:可达性剪枝。走不到的 key = 死者 → 清条目 + 删孤儿组
void TreeProjection::prune() {
    if (m_roots.empty()) {
        return;
    }
    // 账本空 = 没有任何可被剪枝的对象 → 整个 DFS 是纯白走(冷启动首装省一次全树 DFS)
    if (m_byParent.empty() && m_groupOwner.empty()) {
        return;
    }
    PtrSet seen;   // 只做指针值哈希,永不 deref(生存法则不变)
    for (GuiBase* root : m_roots) {
        collectReachable(root, seen);
    }
    std::vector<GuiBase*> stalePages;
    for (const auto& pageEntry : m_byParent) {
        if (!seen.contains(pageEntry.first)) {
            stalePages.push_back(pageEntry.first);
        }
    }
    for (GuiBase* staleOwner : stalePages) {
        dropProjection(staleOwner);   // 对缺失 key 幂等(级联可能已拆掉)
    }
}

// ══ 腿③:声明式绑定(跨全量重建的投影意图) ══

void TreeProjection::bindSpec(SpecBinding binding) {
    if (!binding.locate) {
        return;
    }
    unbindSpec(binding.id);                 // 同 id 覆盖(幂等;内含 ++m_bindGen)
    m_bindings.push_back(std::move(binding));
    ++m_bindGen;                            // 绑定表变了 → 所有"同绑定豁免"作废
    rebind();                               // 注册即生效
}

void TreeProjection::unbindSpec(const std::string& id) {
    m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(),
        [&](const SpecBinding& binding) { return binding.id == id; }), m_bindings.end());
    ++m_bindGen;                            // 表变动 → 豁免作废(防旧 id 撞新绑定)
    // 注:解绑不拆已装投影(保留现场);下次重建后不再重生。要立即拆用 uninstall。
}

// walkBindings 的安装入口:同绑定豁免。
// 页的 (bindId,bindGen) 与当前绑定一致 → Spec 必然一字未动 → 什么都不做
// (页脏不脏由通知/prune 自愈决定,这里不再强制重算 —— v3 的 walk 因此从 1.1s 降到 0.5s)。
void TreeProjection::installFromBinding(GuiBase* node, const SpecBinding& binding) {
    auto pageIt = m_byParent.find(node);
    if (pageIt != m_byParent.end() && pageIt->second.bindGen == m_bindGen
        && pageIt->second.bindId == binding.id) {
        return;                             // 同绑定:页已在,数据变更走通知/自愈通道
    }
    installInternal(node, binding.spec, {});
    pageIt = m_byParent.find(node);         // 盖上出身章(下次 rebind 豁免用)
    if (pageIt != m_byParent.end()) {
        pageIt->second.bindId = binding.id;
        pageIt->second.bindGen = m_bindGen;
    }
}

// rebind 的 DFS:显式栈迭代,语义与旧递归版逐节点一致
void TreeProjection::walkBindings(GuiBase* root, PtrSet& seen) {
    std::vector<GuiBase*> dfsStack;
    dfsStack.push_back(root);
    while (!dfsStack.empty()) {
        GuiBase* node = dfsStack.back();
        dfsStack.pop_back();
        if (!node || !seen.insert(node)) {
            continue;
        }
        // ★ 绑定谓词【不对组节点求值】(不变量#3):先判非组再跑 locate ——
        //   组 label 与 locate 特征同名时会对组装投影 → 无限嵌套(实测崩在 operator new);
        //   且用户 locate 对组做无防御 cast 即 UB(评审 R3:求值顺序也是契约)。
        if (!isGroup(node)) {
            for (auto& binding : m_bindings) {
                if (binding.locate(node)) {
                    installFromBinding(node, binding);
                }
            }
        }
        auto pageIt = m_byParent.find(node);
        if (pageIt != m_byParent.end()) {
            revalidate(node, pageIt->second);
            pageIt = m_byParent.find(node);                 // 组页可能被连锁拆页 → 回查
            if (pageIt == m_byParent.end()) {
                continue;
            }
            for (GuiBase* flatChild : pageIt->second.flat) {
                dfsStack.push_back(flatChild);
            }
        } else if (auto* group = dynamic_cast<TreeGroup*>(node)) {
            if (const auto* memberList = freshMembers(group)) {
                for (GuiBase* member : *memberList) {
                    dfsStack.push_back(member);
                }
            }
        } else {
            size_t rawCount = node->visibleChildrenCount();
            for (size_t i = 0; i < rawCount; ++i) {
                dfsStack.push_back(node->visibleChild(i));
            }
        }
    }
}

// ★ 全量重建后的标准动作(beginFullReset → wipe → prune → rebuild → rebind → endFullReset)
void TreeProjection::rebind() {
    prune();   // 内含全量自愈:重建后的地址复用幽灵/漏通知陈旧页全部在这里收敛
    if (m_bindings.empty() || m_roots.empty()) {
        return;
    }
    PtrSet seen;
    for (GuiBase* root : m_roots) {
        walkBindings(root, seen);
    }
}

void TreeProjection::refresh(GuiBase* parent) {
    prune();
    auto pageIt = m_byParent.find(parent);
    if (pageIt == m_byParent.end()) {
        return;
    }
    pageIt->second.dirty = true;
    revalidate(parent, pageIt->second);
}

// ══ 读侧四件套(ITreeSource)══

size_t TreeProjection::childCount(GuiBase* node) {
    if (!node) {
        return 0;
    }
    auto pageIt = m_byParent.find(node);
    if (pageIt != m_byParent.end()) {
        if (m_paranoid) {
            // 漏报检测(默认 Debug):干净页也枚举比对一次,漂移即点名+自愈
            healIfDrift(node, pageIt->second, /*verbose=*/true);
            pageIt = m_byParent.find(node);          // heal 可能连锁拆组页 → 回查
            if (pageIt == m_byParent.end()) {
                return 0;
            }
        }
        revalidate(node, pageIt->second);            // 脏才重算;干净页零枚举
        pageIt = m_byParent.find(node);              // 组页可能被连锁拆页 → 回查
        return pageIt == m_byParent.end() ? 0 : pageIt->second.flat.size();
    }
    // 没装页 → 原始枚举。组 = owner 成员表就地取 size;普通节点 = 虚接口直读。
    if (auto* group = dynamic_cast<TreeGroup*>(node)) {
        const auto* memberList = freshMembers(group);
        return memberList ? memberList->size() : 0;
    }
    return node->visibleChildrenCount();
}

GuiBase* TreeProjection::child(GuiBase* node, size_t i) {
    // 约定:模型先 childCount(触发重算)再 child(读缓存)
    if (!node) {
        return nullptr;
    }
    auto pageIt = m_byParent.find(node);
    if (pageIt != m_byParent.end()) {
        return i < pageIt->second.flat.size() ? pageIt->second.flat[i] : nullptr;
    }
    if (auto* group = dynamic_cast<TreeGroup*>(node)) {
        const auto* memberList = freshMembers(group);   // O(1) 就地取,不拷成员表
        return (memberList && i < memberList->size()) ? (*memberList)[i] : nullptr;
    }
    return i < node->visibleChildrenCount() ? node->visibleChild(i) : nullptr;
}

// ── locate:parentOf/rowOf 共用的下钻定位(v4 单表版)──────
// 返回 {投影父亲, 行号};行号 -1 = 需要调用方兜底扫描(透传父)或不在投影树(被过滤)。
// 下钻规则(与 v3 parentOf 逐项等价,但查 entries 单表、干净页零枚举):
//   自然父亲(组=owner,数据节点=parentForTree)起,每层:
//     无页 → 组:行号=上一层记下的成员行;透传数据父:row=-1 交 rowOf 扫原始序列
//     有页 → revalidate 后查 entries[node]:
//            查无 = 本页没投影它(被过滤)→ {current,-1}
//            container==current = 直属 flat 行 → {current,row}
//            container==组 → 记住(组,成员行),下钻进组继续(组可能有自己的页,deepest-wins)
std::pair<GuiBase*, int> TreeProjection::locate(GuiBase* node) {
    if (!node) {
        return { nullptr, -1 };
    }
    GuiBase* current = dynamic_cast<TreeGroup*>(node)
                     ? static_cast<TreeGroup*>(node)->owner()   // 组的"自然父亲"=owner
                     : node->parentForTree();                   // 既有虚接口(含 Geom 的 QObject 回退)
    GuiBase* lastContainer = nullptr;   // 上一层页给出的 container(=当前 current 时,lastRow 即答案行号)
    int lastRow = -1;
    while (current) {
        auto pageIt = m_byParent.find(current);
        if (pageIt == m_byParent.end()) {
            // current 无页:① 它是组 → 成员序列就是孩子序列,行号=上一层页记下的成员行;
            //              ② 它是透传数据父 → row=-1,rowOf 兜底扫原始序列(parentOf 不需要行号,免扫)
            if (lastContainer == current) {
                return { current, lastRow };
            }
            return { current, -1 };
        }
        revalidate(current, pageIt->second);
        pageIt = m_byParent.find(current);       // 组页可能被连锁拆页 → 回查
        if (pageIt == m_byParent.end()) {
            return { current, -1 };              // 页没了(组已淘汰):按"无页"处理,rowOf 兜底得 -1
        }
        auto posIt = pageIt->second.entries.find(node);
        if (posIt == pageIt->second.entries.end()) {
            return { current, -1 };              // 本页没投影它(被过滤/已缺席)
        }
        lastContainer = posIt->second.container;
        lastRow = posIt->second.row;
        if (posIt->second.container == current) {
            return { current, lastRow };         // 直属:flat 行
        }
        current = posIt->second.container;       // 进组,继续下钻
    }
    return { nullptr, -1 };
}

GuiBase* TreeProjection::parentOf(GuiBase* node) {
    if (!node) {
        return nullptr;
    }
    return locate(node).first;
}

int TreeProjection::rowOf(GuiBase* node) {
    if (!node) {
        return -1;
    }
    auto location = locate(node);
    if (!location.first) {
        return -1;
    }
    if (location.second >= 0) {
        return location.second;    // entries 直接给出(flat 行 或 组成员行)
    }
    // row=-1 的两种情形:
    if (m_byParent.count(location.first)) {
        return -1;                 // ① 有页但 entries 查无 = 被过滤 → 不在投影树
    }
    // ② 透传父亲(无页):就地扫原始序列
    size_t rawCount = location.first->visibleChildrenCount();
    for (size_t i = 0; i < rawCount; ++i) {
        if (location.first->visibleChild(i) == node) {
            return (int)i;
        }
    }
    return -1;
}
