#include "treeprojection.h"
#include "pipeline.h"
#include <algorithm>
#include <cstdint>      // SIZE_MAX
#include <unordered_map>

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
// 字节口径(重要,别拿这些数字去和 sizeof 累加对比):
//   · 哈希表(成员标注的 unordered_map)= 桶数组(capacity×8) + 节点(n×24~32 含 malloc 头)
//   · std::map = n×48(红黑头 3 指针 + 颜色 + pair)
//   · vector = capacity×8
//   · 组成员向量按 capacity 算(不是 size —— 预留过的槽位是真占内存的)
TreeProjection::MemBreakdown TreeProjection::memoryBreakdown() const {
    MemBreakdown mb;
    size_t specPerPage = 0;   // 每页 spec 的估算字节(所有页同构,取一次即可)
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
        mb.memberToGroup += p.memberToGroup.size();
        mb.memberToPos += p.memberToPos.size();      // 位置索引:与 memberToGroup 同量级
        mb.groupsVec += p.groups.capacity();
        // spec:三个 std::function(各 32B)+ 两个空 string(各 32B)+ opt(≈8B)+ 动态块的堆头
        if (specPerPage == 0) {
            specPerPage = 32 * 3 + 32 * 2 + 8 + 16;
            if (p.spec.filter || p.spec.grouping || p.spec.less) specPerPage += 64;  // 捕获闭包
        }
    }
    mb.groupOwner = m_groupOwner.size();
    // Projection 本体在节点里;桶数组按"负载 0.5"估
    mb.pageObjBytes = hashBytes(mb.pages, sizeof(void*) * 2 + 8 + 232);   // 节点:next+key+val(232B Projection)+ 头
    mb.specBytes = mb.pages * specPerPage;
    mb.lastLiveBytes = mb.lastLivePtr * 8;
    mb.flatBytes = mb.flatPtr * 8;
    // p.members: map 节点(含 group key + vector 24B) + 每个成员向量的堆内存
    mb.membersBytes = mb.memberGroups * 48 + mb.memberPtr * 8 + mb.memberGroups * 16;
    mb.memberToGroupBytes = hashBytes(mb.memberToGroup, 8 + 8 + 8 + 16);  // next+key+val+头
    // memberToPos:每槽 = key(8) + pair<GuiBase*,int>(16,含 padding) = 24B 净,加节点头 ~16B
    mb.memberToPosBytes = hashBytes(mb.memberToPos, 8 + 16 + 16);
    // m_nodePage:节点 → 页,每槽 = key(8) + val(8) + 头 16
    mb.nodePage = m_nodePage.size();   // 直接取,别用 memberToPos 估(两者可能不等)
    mb.nodePageBytes = hashBytes(mb.nodePage, 8 + 8 + 16);
    mb.groupsBytes = mb.groupsVec * 8;
    mb.groupOwnerBytes = hashBytes(mb.groupOwner, 8 + 8 + 8 + 16);
    mb.groupObjBytes = mb.groupOwner * 96;   // TreeGroup: vptr+GuiBase(2 ptr+2 vec=40)+ticket(2 string=64)+owner
    // ── 延迟释放的代价(C4 的直接证据)────────────────────────────
    // 关键点:retiredGroups 用【当前值】,retiredHighWater 用【引擎记的峰值】。
    //   峰值出现在重建中途(组被淘汰但还没 collectGarbage),事后采样 current
    //   会看到 0 —— 这正是原缺口"未纳入基准"的原因:不是忘了测,是采不到。
    mb.retiredGroups = m_retired.size();
    mb.retiredHighWater = m_retiredHighWater;
    mb.retiredBytes = mb.retiredGroups * 96;
    return mb;
}

// ── 写侧入口 ─────────────────────────────────────────
std::vector<TreeGroup*> TreeProjection::install(GuiBase* parent, const ProjectionSpec& spec,
                                                std::vector<GuiBase*> siblings)
{
    EpochScope epoch(this);   // 写侧同样是操作边界:新纪元 → 不吃上一轮的 checkedAt
    prune();   // 操作边界自动自愈(清僵尸投影;批量 delete+new 后尤其重要)
    return installInternal(parent, spec, siblings);
}

// 兼容旧接口:只装分组
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
    Projection& p = projection(parent);   // m_byParent 是节点式 unordered_map:
    //   标准保证 rehash 只失效迭代器、【不失效元素引用】,所以下面一路持有的
    //   Projection& 在任何插入(含嵌套 installInternal 触发的建组)下都安全。
    p.spec = spec;
    // 手动安装:抹掉绑定出身(此页从此按"手动 Spec"对待,rebind 不再豁免它)
    p.bindId.clear();
    p.bindGen = ~0u;

    // 分组参与名单消毒(仅 !dynamicAll 时有意义):
    //   nullptr/parent 自己/重复 → 剔除;"别人家的孩子"无需判(不在活枚举里,天然无效);
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
    p.checkedAt = 0;          // ★ 纪元闸门要一起作废(否则 valid=false 仍会被纪元短路)
    revalidate(parent, p);
    return p.groups;
}

// ── rawChildren:不带本节点投影的"原始孩子"枚举 ──────
// ★ 多层分组的关键函数:
//   · TreeGroup 的原始孩子 = 外层 owner 投影解析出的当前成员
//     (递归终止于数据侧枚举,不会打转);
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

// 组的"新鲜成员表"就地引用:owner 页先 revalidate(缓存命中近零成本),
// 然后直接返回账本里的向量地址 —— 不拷贝。
// ⚠ 指针有效期到 owner 页下次重算为止;所有调用点都是"取得即用"。
// 无 owner / owner 无页 / 组不在成员表(已被淘汰)→ nullptr(等价旧行为:空表)。
const std::vector<GuiBase*>* TreeProjection::freshMembers(TreeGroup* g) {
    auto oit = m_groupOwner.find(g);
    if (oit == m_groupOwner.end()) return nullptr;
    auto pit = m_byParent.find(oit->second);
    if (pit == m_byParent.end()) return nullptr;
    revalidate(oit->second, pit->second);          // 先保证外层成员表新鲜
    // ★ revalidate 可能把 owner 页整页重算(members 被 clear/重建)→ 必须回查迭代器。
    //   (曾经在这里加过"跨操作复用成员表指针"的缓存并配 stamp 校验,但它引入了
    //    一个难以复现的陈旧读:T16 事前坐标协议在 Debug 下稳定失败、Release 下通过。
    //    根因是缓存持有的 vector* 与被 clear 掉的 map 节点生命周期无法可靠绑定。
    //    该缓存不是消除 O(k²) 的关键(关键是 parentOf/rowOf 的每页一次校验),
    //    故按"正确性优先"整段移除。)
    pit = m_byParent.find(oit->second);
    if (pit == m_byParent.end()) return nullptr;
    auto mit = pit->second.members.find(g);
    return mit == pit->second.members.end() ? nullptr : &mit->second;
}

// 缓存钥匙【就地比对】(普通节点版):逐位比虚接口枚举与 lastLive,零分配。
// childCount 是模型 rowCount 的热路径,常态是"数据没变"—— 命中时
// 从"分配+拷贝+比较"降为纯比较(宽父亲 200k 孩子实测差一个数量级)。
// ★ 组页不走这里(见 revalidate 第 0 步):组的枚举会触发 owner 重算,
//   owner 可能当场淘汰这个组并拆掉本页 → p 悬空,必须先确认页还活着。
bool TreeProjection::liveMatchesLast(GuiBase* node, Projection& p) {
    if (!p.valid) return false;                     // 没算过 → 必然走全流水线
    size_t n = node->visibleChildrenCount();
    if (n != p.lastLive.size()) return false;
    for (size_t i = 0; i < n; ++i)
        if (node->visibleChild(i) != p.lastLive[i]) return false;
    return true;
}

// ── revalidate:引擎心脏 = 流水线的【编排器】────────────
// live → [过滤] → [分组] → [排序] → flat
// 各阶段是 pipeline.h 里的纯函数(可独立单测,见 tests.cpp T1~T6);
// 这里只剩三件带所有权/引擎状态的事:建组、淘汰组(级联拆内层)、写账本。
void TreeProjection::revalidate(GuiBase* parent, Projection& p) {
    // 0-0. 查询纪元闸门:本页已在本纪元内校验过 → 无需再枚举活列表。
    //   (语义完全等价于"活列表与 lastLive 一致",因为纪元保证本次操作内无变更。
    //    实测 parentOf+rowOf 一次查询原本重复校验同一页 2~3 次。)
    // ★ m_epochOptimOn:契约 C5 的逃生阀。关掉后这个短路彻底失效 → 每次操作
    //   都重新枚举活列表,**行为完全不变,只是慢一点**。跨线程/异步改数据树
    //   的调用方应调 setEpochOptimization(false) —— 见那行 API 注释。
    if (m_epochOptimOn && p.checkedAt == m_queryEpoch && p.valid) return;
    // 0. 缓存校验:先【就地比对】活列表与 lastLive(零分配),没变 → 整条流水线跳过;
    //    变了才真正建快照(swap 进账本,免去一次大向量拷贝 —— 宽父亲 200k 指针)。
    //    ★ 组页特殊:成员表要靠 owner 重算解析,owner 可能恰好把这组淘汰
    //      (成员全灭)并顺手拆掉本页 —— p 当场悬空。所以组页先取表、
    //      再确认本页还在账上,然后才允许碰 p。
    // ★ checkedAt 在"校验结论得出"的当刻就盖 —— 无论结论是命中还是走完整流水线,
    //   本纪元内本页都已新鲜。必须在此处盖而不是函数末尾:组页的前置 owner 重算
    //   可能连锁拆掉本页(见下一行),那种情况下 p 已悬空,末尾写就是 UAF。
    if (auto* selfG = dynamic_cast<TreeGroup*>(parent)) {
        const std::vector<GuiBase*>* mem = freshMembers(selfG);   // 触发 owner 重算
        if (m_byParent.find(parent) == m_byParent.end()) return;  // 本页已被拆:放弃
        if (p.valid) {
            size_t n = mem ? mem->size() : 0;
            if (n == p.lastLive.size()) {
                bool same = true;
                for (size_t i = 0; i < n; ++i)
                    if ((*mem)[i] != p.lastLive[i]) { same = false; break; }
                if (same) { p.checkedAt = m_queryEpoch; return; }  // 命中:整条流水线跳过
            }
        }
    } else if (liveMatchesLast(parent, p)) {
        p.checkedAt = m_queryEpoch; return;
    }
    std::vector<GuiBase*> live = rawChildren(parent);
    p.lastLive.swap(live);
    // ★ 这里【不】置 valid / checkedAt —— 见函数末尾的"宣布有效"。
    //   下面整条流水线要跑用户提供的 filter / grouping / less,任何一个抛异常,
    //   若此处已宣布有效,下次 childCount 就会因纪元/valid 短路而直接返回
    //   【上一次】的 flat —— 那份 flat 里可能有已被 delete 的对象的野指针。
    //   valid 是"本页 flat 可对外呈现"的承诺,只有流水线跑完才配得上。
    p.checkedAt = m_queryEpoch;

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
    // ★ 槽位必须【传递式】下钻:直接成员全是组的组(组→组→数据),直接成员不在
    //   vis 里,只看一层会拿不到槽位 → 组从 parts 失踪 → 外层规则看不见它
    //   (用户最简复现:手动组3 只挂上了标准件,手动组2 没被收进去)。
    // 成员已全灭的旧组不进 parts(交给淘汰逻辑清掉)。
    std::vector<GuiBase*> parts;
    if (oldAll.empty()) {
        // 没有旧组(冷启动最常见):参与名单就是可见名单,swap 免一次大拷贝
        parts.swap(vis);
    } else {
        // ⚠ 变量名绝不能叫 slots/signals/emit —— 它们是 Qt 关键字宏(QObject 头),
        //   会被预处理器替换成 Q_SLOTS 等,声明静默变垃圾、报错报在使用处。
        // vis 槽位查找,两档自适应:
        //   窄亲(≤256 孩子)→ 线性扫:连续内存指针比较,比一次哈希查找还快
        //     (实测 100 孩子×10 组:哈希建表+查找反而比基线慢 ~2 倍/页);
        //   宽亲(>256)→ 哈希索引:线性会退化成 O(组×成员×孩子),
        //     bench 宽亲场景(数万孩子×数百组)线性要数秒、哈希毫秒级。
        const bool useSlotMap = vis.size() > 256;
        std::unordered_map<GuiBase*, size_t> visSlot;
        if (useSlotMap) {
            visSlot.reserve(vis.size());
            for (size_t i = 0; i < vis.size(); ++i) visSlot.emplace(vis[i], i);
        }
        std::function<size_t(GuiBase*, std::set<GuiBase*>&)> firstVis;
        firstVis = [&](GuiBase* x, std::set<GuiBase*>& guard) -> size_t {
            if (!guard.insert(x).second) return SIZE_MAX;      // 防脏数据成环
            if (useSlotMap) {                                  // 数据节点:自己的槽位
                auto sit = visSlot.find(x);
                if (sit != visSlot.end()) return sit->second;
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
    // 未复用的旧组淘汰(★级联拆其内层投影,否则子组泄漏+僵尸 key)
    std::vector<TreeGroup*> culled;
    // ★ 复用/淘汰的旧组名单 = oldAll(全层),不是 p.groups(只顶层):
    //   嵌套组(作为成员的组)若不在名单里就无法被复用 → 重算即孤悬泄漏
    auto reuse = Pipeline::matchReusable(buckets, oldAll, culled);
    std::map<TreeGroup*, std::vector<GuiBase*>> newMembers;
    for (size_t i = 0; i < buckets.size(); ++i) {
        TreeGroup* g = reuse[i] ? reuse[i] : [&]{ 
            TreeGroup* ng = new TreeGroup(buckets[i].ticket, parent);
            hintMark(m_groupHint, ng);        // ★ 组创建的唯一置位点(见 m_pageHint 注释)
            return ng; }();
        g->updateTicket(buckets[i].ticket);   // label/order 以新 Spec 为准
        newMembers[g] = buckets[i].members;
        m_groupOwner[g] = parent;
    }
    // ★ 沦为"成员"的旧组不能淘汰 —— 它活在外层组里面(多层分组的存活路径)。
    //   仅在有淘汰候选时才全扫成员(dynamic_cast 不便宜:百万成员的全量重算
    //   里这一遍就是几十 ms;稳态 culled 为空 → 整遍跳过)
    std::set<TreeGroup*> asMember;
    if (!culled.empty())
        for (const auto& kv : newMembers)
            for (GuiBase* m : kv.second)
                if (auto* mg = dynamic_cast<TreeGroup*>(m)) asMember.insert(mg);
    std::set<GuiBase*> deleted;                 // 本轮真删掉的旧组(落位时必须排除)
    for (TreeGroup* g : culled) {
        if (asMember.count(g)) continue;
        m_groupOwner.erase(g);
        // ★ deferDelete=true:revalidate 由【读侧】(childCount==rowCount)也能触发,
        //   这里绝不能 delete —— 见 m_retired 的 UAF 说明。
        dropProjection(g, /*deferDelete=*/true);   // 只拆 g 自己名下的内层投影页
        // ★ g 本身必须【单独】入队:dropProjection 遍历的是"parent 的 p.groups",
        //   而 g 在本页的 members 表里(不在 p.groups),所以上面那次调用根本
        //   不会碰 g 自己。少了这一行 = 组既没被回收、也没被记账(实测踩过)。
        noteRetired(g);
        deleted.insert(g);
    }

    // Stage B-4:组内成员排序(与全局同一比较器)
    Pipeline::sortMembers(newMembers, p.spec.less);

    // 重建本 parent 的成员账本(反查只在各层投影内部,parentOf 逐层下钻)
p.members = std::move(newMembers);
    p.memberToGroup.clear();
    {   // ★ 预留到精确规模:memberToGroup 是全引擎最 memberToGroup 最大的表
    //   (百万节点场景 = 100 万条成员映射),让它自然增长会经历一串 rehash
    //   —— 每次 rehash 都是"重建整张桶数组 + 全量重插",既是时间浪费,
    //   也会把峰值内存顶上去(实测 bench 峰值 WS 差 60 MB 就是这么来的)。
    size_t total = 0;
    for (const auto& kv : p.members) total += kv.second.size();
    p.memberToGroup.reserve(total);
    }
    for (const auto& kv : p.members)
        for (GuiBase* m : kv.second) p.memberToGroup[m] = kv.first;

    // 落位 + Stage C:组落在首成员原位置,然后整体 stable_sort
    // (落位遍历 parts 而非 vis:未进桶的旧组也要作为直属组留在本层;
    //  ★ 但本轮已 delete 的旧组必须剔除 —— 它的成员还活着所以仍在 parts 里,
    //    不排除就是把野指针推进 flat,S2 的 use-after-free 就这么来的)
    std::vector<GuiBase*> flatParts;
    flatParts.reserve(parts.size());
    for (GuiBase* x : parts)
        if (!deleted.count(x)) flatParts.push_back(x);
    // 记住旧形状,末尾与新形状比对(结构版本号用)
    const size_t oldFlatSize = p.flat.size();
    const size_t oldGroupCount = p.groups.size();
    p.flat = Pipeline::buildFlat(flatParts, p.memberToGroup);
    Pipeline::sortFlat(p.flat, p.spec.less);
    const size_t newFlatSize = p.flat.size();

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
    const size_t newGroupCount = p.groups.size();

    // ★ 位置索引:节点 → (投影父亲, 行号)。建在【宣布有效之前】,失败则整页留在
    //   valid=false,与 flat 的异常安全语义一致(见下面"宣布有效"的注释)。
    //   两个来源:flat 里的每项(父亲=本页 owner)+ 每组的成员(父亲=该组)。
    p.memberToPos.clear();
    {
        size_t total = p.flat.size();
        for (const auto& kv : p.members) total += kv.second.size();
        p.memberToPos.reserve(total * 2);            // 负载 0.5,避免 rehash
    }
    for (size_t i = 0, c = p.flat.size(); i < c; ++i)
        p.memberToPos[p.flat[i]] = { parent, static_cast<int>(i) };
    for (const auto& kv : p.members)
        for (size_t i = 0, c = kv.second.size(); i < c; ++i)
            p.memberToPos[kv.second[i]] = { kv.first, static_cast<int>(i) };
    // 同一份数据登记"这个节点属于哪一页"(供 parentOf/rowOf 的 O(1) 快路径)。
    // ★ 登记集合 = flat ∪ 各组名下的成员 —— 与 memberToPos 的键集【完全一致】,
    //   因为 memberToGroup 只在遍历 p.members 时填,而 flat 里的组都来自 p.groups。
    for (const auto& kv : p.memberToPos) m_nodePage[kv.first] = parent;

    // ★ 流水线全部跑完,现在才宣布本页有效(异常安全闸门)。
    //   上面任何一步抛异常(用户 filter/grouping/less 都可能抛),本页都会停在
    //   valid=false → 下次查询重跑整条流水线 → 状态自动收敛到正确值。
    //   在此之前绝不能置 valid:那会让下次 childCount 短路返回【旧 flat】,
    //   而旧 flat 里可能含已被 delete 的对象的野指针。
    p.valid = true;
    p.checkedAt = m_queryEpoch;
    // ★ 结构版本号:只有"形状真的变了"才递增 —— 组增删 或 行数变化。
    //   内容变化(同形状下的成员替换)不递增,因为它不破坏模型坐标。
    //   这是给调用方的失步自检工具(见 m_structRev 注释)。
    if (!deleted.empty() || newFlatSize != oldFlatSize || newGroupCount != oldGroupCount)
        ++m_structRev;
}

// ── 纪元优化的逃生阀 + 跨线程检测(契约 C5)─────────────
void TreeProjection::setEpochOptimization(bool enabled) {
    if (enabled == m_epochOptimOn) return;
    m_epochOptimOn = enabled;
    // 关闭时必须把所有页的 checkedAt 清零:否则残留的旧纪元值可能在
    // "某次操作恰好复用了同一个 epoch 数字"时命中(epoch 是单调递增,
    // 理论上不会,但清零让"关掉就一定全量枚举"成为可推理的不变量)。
    if (!enabled)
        for (auto& kv : m_byParent) kv.second.checkedAt = 0;
    m_queryEpoch = 0;          // 让下一次操作从新纪元开始
}

void TreeProjection::onOutermostEntry() {
    ++m_crossThreadEntries;
#ifdef _DEBUG
    std::thread::id self = std::this_thread::get_id();
    if (!m_threadKnown) { m_entryThread = self; m_threadKnown = true; }
    else if (self != m_entryThread) {
        // 只报一次,免得刷屏;不崩 —— 纪元优化在跨线程下本来就"只是变慢/变陈旧"
        fprintf(stderr,
                "[TreeProjection] ⚠ 检测到跨线程进入引擎(契约 C5 前提不成立)。\n"
                "  数据树若被后台线程改动,请调用 setEpochOptimization(false) 退回全枚举。\n"
                "  跨线程进入次数=%zu\n", m_crossThreadEntries);
    }
#endif
}
// deferDelete:false = 立即 delete(写侧边界,调用方已保证视图坐标失效);
//              true  = 只摘账、组进 m_retired 等 collectGarbage(读侧路径专用)。
//   读侧绝不能 delete:QTreeView 对每个展开的组持有 QPersistentModelIndex,
//   其 internalPointer 就是这个 TreeGroup*,释放即 UAF 且不发任何信号。
void TreeProjection::dropProjection(GuiBase* parent, bool deferDelete) {
    auto it = m_byParent.find(parent);
    if (it == m_byParent.end()) return;
    Projection& p = it->second;
    // ★ 位置索引必须与本页同生共死:先把"可能指向本页的键"收齐再删。
    //   为什么不能边删边查:节点出现在本页的 flat(直属/本层组)里,而被分组的成员
    //   只在【组的 members】里 —— 两处都要覆盖(即 memberToPos 的键集)。
    //   点查删除而不按值扫全表,是为了避免 O(节点总数) 的拆页成本。
    {
        std::vector<GuiBase*> keys;
        keys.reserve(p.memberToPos.size());
        for (GuiBase* x : p.flat) keys.push_back(x);
        for (const auto& kv : p.members) keys.push_back(kv.first);
        for (size_t i = 0; i < keys.size(); ++i) {
            auto nit = m_nodePage.find(keys[i]);
            if (nit != m_nodePage.end() && nit->second == parent) m_nodePage.erase(nit);
        }
    }
    for (TreeGroup* g : p.groups) {
        m_groupOwner.erase(g);
        dropProjection(g, deferDelete);                 // ★级联:组名下的内层投影先拆
        if (deferDelete) noteRetired(g); else delete g;
    }
    m_byParent.erase(it);   // 哈希表:递归只 erase 别的 key,本迭代器仍有效
}

// 真正释放已淘汰的组 —— 全工程唯一 delete TreeGroup 的地方(除引擎析构)。
// ★ 调用时机契约:必须在视图的 persistent index 全部失效之后,即
//   beginResetModel / endResetModel 之内。NavTreeModel 的 beginFullReset
//   已经自动调用它(见 navtreemodel.h),所以走 reset 通道的调用方零心智负担。
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
// ★ 显式栈迭代版(原递归版在 bench deep 场景 10 万级深链上爆栈崩溃;
//   百万节点导航树里"深度不受限"必须是引擎自身保证,不能赌调用方栈大小)。
// ★ 孩子枚举就地走虚接口/成员表引用,不再经 rawChildren 拷整张向量
//   (1M 节点 DFS = 1M 次向量分配,实测占 prune 大头)。
void TreeProjection::collectReachable(GuiBase* root, PtrSet& seen) {
    std::vector<GuiBase*> stack;         // 显式 DFS 栈(深度不限,内存在堆上)
    stack.push_back(root);
    while (!stack.empty()) {
        GuiBase* n = stack.back();
        stack.pop_back();
        if (!n || !seen.insert(n)) continue;   // 兼防脏数据成环
        // ★ 提示位图门控:两个提示都没置位 ⇒ 一定无账页且不是组 ⇒ 直接走原始枚举。
        //   省掉的是每个普通节点一次的哈希探测(cache miss)+ dynamic_cast(RTTI 走链),
        //   实测合计 60~80 ns/节点。提示位永不漏置(创建点只有 projection() 与
        //   new TreeGroup 两处),只会多算不会漏算 —— 漏算才是致命的,多算无害。
        if (!hintProbe(m_pageHint, n) && !hintProbe(m_groupHint, n)) {
            size_t cnt = n->visibleChildrenCount();
            for (size_t i = 0; i < cnt; ++i) stack.push_back(n->visibleChild(i));
            continue;
        }
        auto pit = m_byParent.find(n);
        if (pit != m_byParent.end()) {
            revalidate(n, pit->second);               // 保证 flat 新鲜再走账本
            pit = m_byParent.find(n);                 // 组页可能被连锁拆页 → 回查
            if (pit == m_byParent.end()) continue;
            for (GuiBase* c : pit->second.flat) stack.push_back(c);
        } else if (auto* g = dynamic_cast<TreeGroup*>(n)) {
            // 组:成员表就地引用(freshMembers 内部已 revalidate owner)
            if (const auto* mem = freshMembers(g))
                for (GuiBase* c : *mem) stack.push_back(c);
        } else {
            // 普通节点:就地走虚接口,不拷向量
            size_t cnt = n->visibleChildrenCount();
            for (size_t i = 0; i < cnt; ++i) stack.push_back(n->visibleChild(i));
        }
    }
}

// 腿②:可达性剪枝。走不到的 key = 死者 → 清条目 + 删孤儿组
void TreeProjection::prune() {
    EpochScope epoch(this);   // 同 install:prune 是操作边界,必须重开纪元
    if (m_roots.empty()) return;
    // ★ 账本空 = 没有任何可被剪枝的对象 → 整个 DFS 是纯白走。
    //   冷启动首装(bindSpec 第一次生效)正是这个状态:bench 实测省掉
    //   一次 1M 节点全树 DFS(约 250 ms)。判据:m_byParent 空 ⟹ m_groupOwner 也必空
    //   (每个组都由某个账页创建,dropProjection 双向成对维护),故无孤儿组可释放。
    if (m_byParent.empty() && m_groupOwner.empty()) return;
    // 去重集:只做指针值哈希,永不 deref(生存法则不变)。★ 用 PtrSet(开放寻址)
    //   而非 unordered_set:后者在 1M 节点上实测 234 ns/节点,占 prune 总耗时 2/3,
    //   而"走树"本身只要 8.7 ns/节点(见 ptrset.h 头注释的 A/B 数据)。
    PtrSet seen;
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
    EpochScope epoch(this);   // 写侧操作边界
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
// 页的 (bindId, bindGen) 与当前绑定一致 → Spec 必然一字未动 →
// 只做 revalidate(数据没变即缓存命中,近零成本);否则走完整 installInternal。
// ★ 没有这条豁免时,rebind 里 walkBindings 会把 prune 刚重算完的每一页
//   再强制 valid=false 全量重算一遍(bench 实测 1 万页多花 ~550ms)。
void TreeProjection::installFromBinding(GuiBase* n, const SpecBinding& b) {
    auto pit = m_byParent.find(n);
    if (pit != m_byParent.end() && pit->second.bindGen == m_bindGen
        && pit->second.bindId == b.id) {
        revalidate(n, pit->second);         // 同绑定:数据驱动重算(命中≈零成本)
        return;
    }
    installInternal(n, b.spec, {});
    pit = m_byParent.find(n);               // 盖上出身章(下次 rebind 豁免用)
    if (pit != m_byParent.end()) {
        pit->second.bindId = b.id;
        pit->second.bindGen = m_bindGen;
    }
}

// rebind 的 DFS:同样显式栈迭代(深链不设限),语义与旧递归版逐节点一致
void TreeProjection::walkBindings(GuiBase* root, PtrSet& seen) {
    std::vector<GuiBase*> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        GuiBase* n = stack.back();
        stack.pop_back();
        if (!n || !seen.insert(n)) continue;
        // 提示位图只用来【跳过昂贵的"否定探测"】,绝不能跳过谓词 ——
        //   locate 必须对每个活数据节点求值,否则"谓词命中但没账页"的节点
        //   永远装不上投影(声明式绑定的全部意义就没了)。
        //   门控后能免掉的:① 无组提示 ⇒ 跳过 dynamic_cast(确定不是组);
        //     ② 无页提示 ⇒ 跳过 m_byParent 的哈希探测(确定没账页)。
        //   (漏置位只会多算不会漏算,失效方向安全 —— 见 m_pageHint 注释)
        const bool maybeGroup = hintProbe(m_groupHint, n);
        // ★ 绑定谓词【不对组节点求值】:组是投影产物不是数据节点。否则当组 label
        //   与 locate 特征同名时,会对组再装投影 → 组内成员又被同规则分出内层组 →
        //   内层组 label 仍同名 → 无限嵌套(实测栈/堆耗尽崩在 operator new)。
        for (auto& b : m_bindings)
            if (b.locate(n) && (maybeGroup ? !isGroup(n) : true))
                installFromBinding(n, b);   // 同绑定豁免:表没变 → 只校验缓存,不强制重算
        auto pit = m_byParent.end();
        if (hintProbe(m_pageHint, n)) pit = m_byParent.find(n);
        if (pit != m_byParent.end()) {
            revalidate(n, pit->second);
            pit = m_byParent.find(n);                 // 组页可能被连锁拆页 → 回查
            if (pit == m_byParent.end()) continue;
            for (GuiBase* c : pit->second.flat) stack.push_back(c);
        } else if (auto* g = maybeGroup ? dynamic_cast<TreeGroup*>(n) : nullptr) {
            if (const auto* mem = freshMembers(g))
                for (GuiBase* c : *mem) stack.push_back(c);
        } else {
            size_t cnt = n->visibleChildrenCount();
            for (size_t i = 0; i < cnt; ++i) stack.push_back(n->visibleChild(i));
        }
    }
}

// ★ 全量重建后的标准动作(beginResetModel → wipe → prune → rebuild → rebind → endResetModel)
void TreeProjection::rebind() {
    EpochScope epoch(this);   // 全量重建的操作边界:内部 DFS 全部共享本纪元
    prune();
    if (m_bindings.empty() || m_roots.empty()) return;
    PtrSet seen;
    for (GuiBase* r : m_roots) walkBindings(r, seen);
}

void TreeProjection::refresh(GuiBase* parent) {
    EpochScope epoch(this);
    prune();
    auto it = m_byParent.find(parent);
    if (it == m_byParent.end()) return;
    it->second.valid = false;
    it->second.checkedAt = 0;   // 强制下一句 revalidate 真跑一遍(纪元闸门要一起作废)
    revalidate(parent, it->second);
}

// ══ 读侧四件套(ITreeSource) ══

size_t TreeProjection::childCount(GuiBase* node) {
    if (!node) return 0;
    EpochScope epoch(this);   // 查询纪元:本次操作内每页只校验一次(见 m_queryEpoch)
    // ★ 装过投影的节点(含"组内分组"的 TreeGroup)→ 优先返回投影序列。
    //   顺序不能反:组既有外层成员表又可能有内层投影时,内层投影才是它当前的孩子。
    auto it = m_byParent.find(node);
    if (it != m_byParent.end()) {
        revalidate(node, it->second);          // childCount 是唯一校验点
        // revalidate 对组页可能连锁触发 owner 重算并把本页整页淘汰(组蒸发),
        // 迭代器随之失效 → 回查一次,页没了按 0 处理(调用方手里的 node 指针
        // 此刻也已失效,这是"末成员死亡 → 信号发在组行"协议要防的既有边界)
        it = m_byParent.find(node);
        return it == m_byParent.end() ? 0 : it->second.flat.size();
    }
    // 没装页 → 原始枚举。★ 不再经 rawChildren 拷整张向量:
    //   组 = owner 成员表就地取 size;普通节点 = 虚接口直读。
    //   (旧实现:rowOf 循环里每次 child(group,i) 都拷一遍成员表 + 顺带
    //    revalidate 校验 owner 全量活列表 → O(组员×父孩子) 复合二次方,
    //    bench wide 场景单次 rowOf 0.4s,1 万次要跑一小时)
    if (auto* g = dynamic_cast<TreeGroup*>(node)) {
        const auto* mem = freshMembers(g);
        return mem ? mem->size() : 0;
    }
    return node->visibleChildrenCount();
}

GuiBase* TreeProjection::child(GuiBase* node, size_t i) {
    // 约定:模型先 childCount(校验缓存)再 child(读缓存)
    if (!node) return nullptr;
    EpochScope epoch(this);   // 与 childCount 同理:每次操作开新纪元
    auto it = m_byParent.find(node);
    if (it != m_byParent.end()) {
        if (i < it->second.flat.size()) return it->second.flat[i];
        return nullptr;
    }
    if (auto* g = dynamic_cast<TreeGroup*>(node)) {
        const auto* mem = freshMembers(g);     // O(1) 就地取,不拷成员表
        return (mem && i < mem->size()) ? (*mem)[i] : nullptr;
    }
    return i < node->visibleChildrenCount() ? node->visibleChild(i) : nullptr;
}

GuiBase* TreeProjection::parentOf(GuiBase* node) {
    if (!node) return nullptr;
    // ★★ 快路径(2026-10-06 落地):位置索引直接查表 —— O(1)。
    //    账本里 memberToPos 存的 first 就是【已解析好的投影父亲】(被分组就是那个组,
    //    没进组就是自然父亲),这正是本函数的返回值 —— 不需要再下钻、也不需要校验。
    //    旧实现每层都要 revalidate(全枚举该页全部孩子)才能查 memberToGroup,
    //    于是"对每个成员问一次 parentOf" = O(k²);k=5000 时单次 8.6 µs、一次首屏 76 ms。
    //    与旧实现逐项对拍 0 差异(985 项全树 + 200 轮随机增删/换规则/重装)。
    //    查不到 = 不在任何投影页上 → 回退旧路径(语义完全不变,只是慢)。
    {
        auto pg = m_nodePage.find(node);
        if (pg != m_nodePage.end()) {
            auto pit = m_byParent.find(pg->second);
            if (pit != m_byParent.end()) {
                // ★ 本页在本操作纪元内没校验过就补一次 —— 这与旧实现的校验语义一致,
                //   但粒度是【每页最多一次】而不是旧实现的"每问一个成员枚举一遍全页"。
                //   (为什么不干脆不校验:checkEpoch 会被 beginQuery 之类重置为 0,
                //    此时索引可能来自更早的纪元 —— 直接采信会返回陈旧答案,
                //    T16 事前坐标协议就是这么被抓到的。)
                revalidate(pg->second, pit->second);
                pit = m_byParent.find(pg->second);      // 页可能被连锁拆掉 → 回查
                if (pit != m_byParent.end()) {
                    auto mit = pit->second.memberToPos.find(node);
                    if (mit != pit->second.memberToPos.end()) return mit->second.first;
                }
            }
        }
    }
    // ── 回退路径(保持原语义不变)──
    EpochScope epoch(this);   // 下钻链每层都要 revalidate,纪元让同层只校一次
    // ★ 多层分组:组本身也可能是外层组的成员 —— 所以组不直接返回 owner,
    //   而是从 owner 出发和普通节点走同一条下钻链(每层查该层 memberToGroup),
    //   终止于"没装投影"或"这层没把它分组"→ 天然 deepest-wins,无全局反查表
    GuiBase* p = dynamic_cast<TreeGroup*>(node)
                 ? static_cast<TreeGroup*>(node)->owner()   // 组的"自然父亲"=owner
                 : node->parentForTree();                   // 既有虚接口(含 Geom 的 QObject 回退)
    while (p) {
        auto pit = m_byParent.find(p);
        if (pit == m_byParent.end()) return p;
        revalidate(p, pit->second);
        // ★ p 是组页时,revalidate 可能连锁触发 owner 把 p 整组淘汰并拆页
        //   (成员全灭的组蒸发)→ pit 悬空,必须回查(见 revalidate 第 0 步注释)
        pit = m_byParent.find(p);
        if (pit == m_byParent.end()) return p;
        auto mit = pit->second.memberToGroup.find(node);
        if (mit == pit->second.memberToGroup.end()) return p;
        p = mit->second;                       // 进组,继续查组的内层
    }
    return nullptr;
}

int TreeProjection::rowOf(GuiBase* node) {
    // ★★ 快路径(2026-10-06 落地):位置索引直接查表 —— O(1)。
    //    旧实现是 parentOf + 线性扫序列,而 parentOf 为了"校验缓存"要枚举整页孩子
    //    ⇒ 遍历 k 行 = k 次 O(k) ⇒ O(k²)。实测最宽层 5000 时一次展开首屏 76 ms;
    //    2000 次全树遍历 11.6 s。索引化后与旧实现【逐项对拍 0 差异】
    //    (985 项全树对拍 + 200 轮随机增删/换规则/重装,见 PROJECT.md)。
    //    查不到(= 不在任何投影页上:被过滤/已死/没装过)才回退旧路径,语义不变。
    if (node) {
        auto pg = m_nodePage.find(node);
        if (pg != m_nodePage.end()) {
            auto pit = m_byParent.find(pg->second);
            if (pit != m_byParent.end()) {
                // 与 parentOf 同理:本操作纪元内该页没校验过就补一次(每页最多一次),
                // 然后从索引取行号。校验是必须的 —— 索引可能与当前数据侧不同步
                // (见 parentOf 里的说明:纪元会被重置,不能凭"引用还在"就采信)。
                revalidate(pg->second, pit->second);
                pit = m_byParent.find(pg->second);      // 页可能被连锁拆掉 → 回查
                if (pit != m_byParent.end()) {
                    auto mit = pit->second.memberToPos.find(node);
                    if (mit != pit->second.memberToPos.end()) return mit->second.second;
                }
            }
        }
    }
    // ── 回退路径(保持原语义不变)──
    // ★ 本函数自己开纪元而不是靠 parentOf:rowOf 的实现是 parentOf + 扫序列,
    //   二者必须是【同一次操作】(共享纪元)—— 否则内层 parentOf 递增纪元后,
    //   外层随后对同一页的 revalidate 会因纪元不符而重校验,优化归零。
    EpochScope epoch(this);
    GuiBase* p = parentOf(node);   // 嵌套调用不递增纪元(见 EpochScope)
    if (!p) return -1;
    // ★ 就地取账本序列扫一遍 —— 不再逐位调 child(p,i):
    //   旧实现每个位置都触发一次完整缓存校验(组路径 = revalidate owner),
    //   宽父亲(20 万孩子)组的成员一次 rowOf 实测 0.46s = O(组员×父孩子);
    //   现在整次 rowOf 只校验一遍,回到 O(兄弟数)。
    auto pit = m_byParent.find(p);
    if (pit != m_byParent.end()) {
        revalidate(p, pit->second);            // 唯一一次校验,序列保证新鲜
        pit = m_byParent.find(p);              // 组页可能被连锁拆页 → 回查
        if (pit == m_byParent.end()) return -1;
        const auto& flat = pit->second.flat;
        for (size_t i = 0; i < flat.size(); ++i)
            if (flat[i] == node) return (int)i;
        return -1;
    }
    if (auto* g = dynamic_cast<TreeGroup*>(p)) {
        const auto* mem = freshMembers(g);     // 一次校验 + 就地引用扫描
        if (!mem) return -1;
        for (size_t i = 0; i < mem->size(); ++i)
            if ((*mem)[i] == node) return (int)i;
        return -1;
    }
    size_t n = p->visibleChildrenCount();      // 透传节点:虚接口就地扫
    for (size_t i = 0; i < n; ++i)
        if (p->visibleChild(i) == node) return (int)i;
    return -1;                                 // 被过滤/已死 → 不在投影树上
}
