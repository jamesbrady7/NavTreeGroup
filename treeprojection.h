#pragma once
// ═══════════════════════════════════════════════════════════════
//  TreeProjection:展示树投影引擎(v4 · 脏通知契约)
// ═══════════════════════════════════════════════════════════════
//  身份:数据侧原始枚举 → 展示树的【唯一加工点】。分组只是流水线的一节,
//  过滤/排序与它平级。模型只通过 ITreeSource 四件套读投影结果。
//
//  ★ v4 的核心交换(详见 docs/重构v4_脏通知.md):
//    v3 靠"每次读都枚举活列表比对 lastLive"来【惰性发现】数据变更 ——
//    为压住这个成本堆了纪元闸门/位置双索引/提示位图三层加速机械,
//    每层都有自己的失效边界(R1/R2/C8 三起事故都出在这),代码难读难调。
//    v4 改为【通知失效】:调用方在数据变更后调一次 invalidate(O(1) 置脏),
//    读侧只查脏标志 —— 干净=直接答缓存(零枚举),脏=重算一次。
//      · 跨调用 O(k²) 根除(v3 文档承认只有数据侧信号这一条路能治);
//      · 缓存检查从三十行机械塌缩成一行 `if (p.valid && !p.dirty) return;`;
//      · 代价:漏通知=投影陈旧,由三层兜底收敛(见下"漏通知兜底")。
//
//  设计根基(与 v2/v3 一致):
//   · 组不存成员:成员关系 = 规则 × 父亲当前活列表,现场重算(惰性投影);
//   · 对现有数据对象零写入(连 m_parentForTree 都不碰);
//   · 三政体生存:discard 讣告 / registerRoot+prune 可达性剪枝 /
//     bindSpec+rebind 声明式规则跨全量重建自动重生。
//
//  流水线语义(不变):
//   live = rawChildren(parent)  → [过滤] → [分组:组建在首成员原位、复用旧组保状态]
//   → [排序:less 对"组+直属"混排 stable_sort] = flat(模型看到的投影序列)
//
//  ── 漏通知的三层兜底(内网可调试性的正面回答)──
//   ① prune/rebind 自愈(Release 也在):DFS 到每个账页做"枚举 vs lastLive"比对,
//      漂移即置脏重算 → 忘了通知,陈旧最多活到下一次卫生边界(install/refresh/rebind 都带 prune)。
//   ② paranoid 漏报检测(默认 _DEBUG 开):childCount 对干净页额外比对,漂移即
//      stderr 点名"哪个父节点漏了 invalidate"并当场自愈 → 开发期一跑 demo 就暴露漏报点。
//   ③ structRev():调用方精准操作前后各取一次,形状漂移当场可见。
#include "guibase.h"
#include "itreesource.h"
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "ptrset.h"      // prune/rebind 的 DFS 去重集(实测比 unordered_set 快 20 倍)

// ── 票据:组的身份 + 展示信息 ──────────────────────────
struct GroupTicket {
    std::string key;      // 相同 key 归一组;空 key = 不分组(逃生口)
    std::string label;    // 组显示名,空则回退 key
    int order = 0;        // 组间排序提示(供 less 比较器消费)
};

// ── 分组规则:纯函数 节点 → 可选票据(可反复重算) ──
using GroupRule = std::function<std::optional<GroupTicket>(GuiBase*)>;

struct GroupingOptions {
    bool collapseIfSingleGroup = false;  // 全员同组 → 不建废层级
    bool dynamicAll = false;             // true:参与集=当前全部活孩子(新节点自动进组)
};

// ── 投影规格:一个父亲的完整投影定义(三个槽位都可为空) ──
struct ProjectionSpec {
    std::function<bool(GuiBase*)> filter;                 // false → 移出投影;空 = 不过滤
    GroupRule grouping;                                   // 空 = 不分组
    std::function<bool(GuiBase*, GuiBase*)> less;         // 严格弱序;空 = 保持数据侧顺序
    GroupingOptions opt;
};

class TreeProjection;

// ── 展示组节点:零数据实体、零所有权、不存成员 ─────────
class TreeGroup : public GuiBase {
public:
    TreeGroup(GroupTicket t, GuiBase* owner)
        : GuiBase(nullptr), m_ticket(std::move(t)), m_owner(owner) {}

    std::string type() override { return "TreeGroup"; }
    GuiBase* parentForTree() const override { return m_owner; }

    // 成员枚举全部委托引擎现场解析 → 本对象内部无指针可悬空
    size_t visibleChildrenCount() const override;
    GuiBase* visibleChild(size_t index) const override;

    const GroupTicket& ticket() const { return m_ticket; }
    void updateTicket(GroupTicket t) { m_ticket = std::move(t); }
    GuiBase* owner() const { return m_owner; }

private:
    // m_ticket:组身份三件套(key=归组依据/label=显示名/order=组间排序)。
    //   写:构造时;revalidate 复用旧组按 key 匹配 → 同 key 复用同一指针,
    //     QModelIndex/展开/选中状态跨刷新存活。读:labeler、less。
    GroupTicket m_ticket;
    // m_owner:原父亲(挂载点),非持有、不管理生死。
    //   读:parentForTree()(故 parentOf(组) 从 owner 起下钻)、rawChildren(组) 找 owner 页解析成员。
    //   ⚠ owner 死后此指针变野,但引擎只拿它做 map key 值比较、永不 deref —— prune 能在死者身上工作的生存前提。
    GuiBase* m_owner;
};

// ── 位置条目:每页唯一的"节点 → 投影坐标"表(v4 单表,替代 v3 的 memberToGroup+memberToPos+m_nodePage)──
//   container = 投影父亲:页 owner 本身(直属落位)或某个组(被分组);
//   row       = 在 container 孩子序列里的行号(flat 行 或 members 行)。
//   键集 = flat ∪ 各组成员;随页同生共死(无全局副本 → v3 的 R2"登记/清理不对称"在结构上不可能再发生)。
struct PosEntry {
    GuiBase* container = nullptr;
    int      row = -1;
};

// ── 投影引擎 ──────────────────────────────────────────
class TreeProjection : public ITreeSource {
public:
    static TreeProjection& instance();
    ~TreeProjection();   // 释放全部 TreeGroup(引擎是组的唯一所有者)

    using Options = GroupingOptions;   // 兼容旧名

    // ══ 写侧 API ══
    std::vector<TreeGroup*> install(GuiBase* parent, const ProjectionSpec& spec,
                                    std::vector<GuiBase*> siblings = {});
    std::vector<TreeGroup*> apply(GuiBase* parent, const std::vector<GuiBase*>& siblings,
                                  GroupRule rule, Options opt = {});
    // ── 写侧三个"立即释放组"的入口(前提:调用方已保证视图坐标失效,即在 reset 括号内)──
    //   uninstall/discard/prune 走 dropProjection(...,deferDelete=false)= 当场 delete。
    //   ⚠ 读侧与 revalidate 淘汰分支【绝不】走这里(它们 deferDelete=true,见 collectGarbage 契约)。
    void uninstall(GuiBase* parent);
    void ungroup(GuiBase* parent) { uninstall(parent); }
    void refresh(GuiBase* parent);     // = invalidate(parent) + 立即重算(幂等收敛)
    bool hasProjection(GuiBase* parent) const;
    size_t liveGroupCount() const;

    // ══ ★数据侧变更通知(v4 使用侧的唯一新义务)══════════════════
    //   契约:动完数据就调一次(幂等、O(1) 置脏、可在批量操作末尾统一调)。
    //   通知只置脏标志,不触发重算 —— 重算发生在下次读(批量变更天然合并)。
    //   漏通知不会崩,只会投影陈旧,并由 prune/paranoid 三层兜底收敛(见文件头)。
    void invalidate(GuiBase* parent);       // parent 的孩子列表变了(增/删/移动/重排序)
    void invalidateNode(GuiBase* node);     // node 自身属性变了(材质/名字等影响 filter/grouping/less 的值)
    void notifyRemove(GuiBase* parent, GuiBase* dying);
        // 便捷组合 = invalidate(parent) + discard(dying)。必须在 delete dying【之前】调。
        // 级联删整棵子树:只对子树根调一次即可(内部页变僵尸,prune 清)。
    void invalidateAll();                   // 全部账页置脏(大变更后不想逐个报;下次读逐页收敛)

    // ══ 组回收:读侧绝不 delete,只有这里才真正释放(契约 C1)══════
    //   淘汰的组只进 m_retired(账本已摘除、内存仍有效),由 collectGarbage() 在
    //   【视图 persistent index 全部失效之后】(= begin/endReset 或 begin/endRemoveRows 之内)真正 delete。
    //   NavTreeModel 的 endFullReset/endRowRemoval 已自动调用 → 走这两条通道的调用方零心智负担。
    void   collectGarbage() override;
    size_t retiredCount() const { return m_retired.size(); }
    bool   hasRetired() const { return !m_retired.empty(); }

    // ── 结构版本号(契约 C9 失步自检;递增条件:组增删 或 flat 行数变)──
    size_t structRev() const override { return m_structRev; }

    // ── paranoid 漏报检测(默认 _DEBUG 开、Release 关)──────────────
    //   开:childCount 对干净页额外做"枚举 vs lastLive"比对,漂移即 stderr 点名 + 自愈。
    //   不是性能开关,是【开发期抓漏通知】的工具;Release 关掉省那一次枚举。
    void setParanoidChecks(bool on) { m_paranoid = on; }
    bool paranoidChecks() const { return m_paranoid; }

    // ══ 账本内存分解(纯观测)══════════════════════════════════
    struct MemBreakdown {
        size_t pages = 0;          // 账页数(= m_byParent 条目)
        size_t specPerPage = 0;
        size_t poolNodes = 0;
        size_t lastLivePtr = 0;    // lastLive 缓存快照指针总数(自愈比对用)
        size_t flatPtr = 0;
        size_t memberGroups = 0;   // p.members 的组条目数
        size_t memberPtr = 0;      // p.members 里成员指针总数
        size_t entries = 0;        // ★ p.entries 条目总数(= flat ∪ 各组成员,单一位置表)
        size_t groupsVec = 0;
        size_t groupOwner = 0;
        size_t pageObjBytes = 0;
        size_t lastLiveBytes = 0;
        size_t flatBytes = 0;
        size_t membersBytes = 0;
        size_t entriesBytes = 0;   // ★ 单表字节(替代 v3 的 memberToGroup+memberToPos+nodePage 三张)
        size_t groupsBytes = 0;
        size_t groupOwnerBytes = 0;
        size_t specBytes = 0;
        size_t totalBytes() const {
            return pageObjBytes + specBytes + poolNodes * sizeof(void*) * 2
                 + lastLiveBytes + flatBytes + membersBytes
                 + entriesBytes + groupsBytes + groupOwnerBytes;
        }
        size_t groupObjBytes = 0;
        size_t retiredGroups = 0;
        size_t retiredHighWater = 0;
        size_t retiredBytes = 0;
        size_t totalWithRetired() const { return totalBytes() + retiredBytes; }
    };
    MemBreakdown memoryBreakdown() const;
    size_t retiredHighWater() const { return m_retiredHighWater; }
    void   resetRetiredHighWater() { m_retiredHighWater = m_retired.size(); }
    // 组入队待回收 + 更新峰值水位。**所有 m_retired 写入必须走它**(直接 push_back 会漏记高水位)。
    void noteRetired(TreeGroup* g) {
        m_retired.push_back(g);
        if (m_retired.size() > m_retiredHighWater) m_retiredHighWater = m_retired.size();
    }

    // ══ 生命周期卫生(三政体生存法则)══
    void discard(GuiBase* dying);      // 腿①:显式讣告(delete 前调;纯 map 操作,不 deref 死者)
    void registerRoot(GuiBase* root);
    void unregisterRoot(GuiBase* root);
    void prune();                      // 腿②:可达性剪枝 + 漏通知自愈(僵尸投影清除+孤儿组释放)

    // ══ 腿③:声明式 Spec 绑定(跨全量重建重生)══
    struct SpecBinding {
        std::string id;
        std::function<bool(GuiBase*)> locate;
        ProjectionSpec spec;
    };
    void bindSpec(SpecBinding b);
    void unbindSpec(const std::string& id);
    void rebind();                     // ★全量重建后必调:prune(含自愈)+ 按谓词重装全部绑定

    // ══ 读侧 API(ITreeSource 实现,模型唯一入口)══
    //   v4:干净页零枚举直接答缓存;脏页重算一次。跨调用不再 O(k²)。
    size_t   childCount(GuiBase* node) override;
    GuiBase* child(GuiBase* node, size_t i) override;
    GuiBase* parentOf(GuiBase* node) override;     // 逐层下钻 deepest-wins(走 entries 单表)
    int      rowOf(GuiBase* node) override;
    bool     isGroup(GuiBase* node) const override;

private:
    TreeProjection() = default;
    TreeProjection(const TreeProjection&) = delete;
    TreeProjection& operator=(const TreeProjection&) = delete;

    // 一个父亲的完整投影状态(账本的一页)。
    struct Projection {
        // spec:本页投影定义(filter/grouping/less/opt)。写:install/bindSpec;读:revalidate 消费。
        ProjectionSpec spec;
        // pool:分组静态参与名单(仅 dynamicAll=false 生效)。写:install(siblings) 消毒后;读:分组阶段。
        std::set<GuiBase*> pool;
        // lastLive:上次重算时 rawChildren 的快照。★v4 角色变了 —— 不再是每次读的比对钥匙,
        //   而是【自愈/漏报检测】的参照:prune 与 paranoid 用它发现"数据变了但没收到通知"。
        //   写:revalidate 重算时 swap 进来;读:healIfDrift 比对。
        std::vector<GuiBase*> lastLive;
        // flat:流水线最终产物(组+直属混排,组在首成员原位)。写:revalidate;读:childCount/child/rowOf。
        std::vector<GuiBase*> flat;
        // groups:当前生效的组列表(引擎是唯一所有者)。写:revalidate;读:uninstall/析构/传播置脏。
        std::vector<TreeGroup*> groups;
        // members:组 → 活成员列表(已按 less 排序)。写:revalidate;读:freshMembers(组成员关系唯一存储处)。
        std::map<TreeGroup*, std::vector<GuiBase*>> members;
        // ★entries:本页唯一的"节点 → 投影坐标"表(v4 单表,替代 memberToGroup+memberToPos)。
        //   键集 = flat ∪ 各组成员;container=投影父亲(owner 或组),row=在其孩子序列里的行号。
        //   写:revalidate 末尾两阶段(先成员行、后 flat 行);读:locate(parentOf/rowOf 共用)、buildFlat 的 rep 上溯。
        //   随页同生共死 → 无全局副本 → 不存在 v3 R2 的"登记/清理不对称"泄漏。
        std::unordered_map<GuiBase*, PosEntry> entries;
        // valid:账页是否已算过且 flat 可对外呈现。revalidate 流水线【全部跑完】才置位(异常安全闸门:
        //   中途用户 filter/grouping/less 抛异常 → 停在 valid=false → 下次重查自动收敛,绝不短路返回含野指针的旧 flat)。
        bool valid = false;
        // ★dirty:数据侧已变更、等待重算。写:invalidate 族 / revalidate 传播 / healIfDrift;
        //   读:revalidate 第 0 行 `if (valid && !dirty) return;` —— 这就是 v4 的全部缓存检查。
        bool dirty = true;
        // 绑定出身证明(walkBindings 同绑定豁免用):同 id 同代号 → Spec 没变 → 免强制重算。
        std::string bindId;
        unsigned bindGen = ~0u;
    };

    // ★账页唯一创建点(v4 不再需要位图置位,建页就是这一行 operator[])。
    Projection& projection(GuiBase* parent) { return m_byParent[parent]; }
    std::vector<TreeGroup*> installInternal(GuiBase* parent, const ProjectionSpec& spec,
                                            const std::vector<GuiBase*>& siblings);
    void installFromBinding(GuiBase* n, const SpecBinding& b);
    void revalidate(GuiBase* parent, Projection& p);   // 脏才重算:过滤→分组→排序→建 entries
    void dropProjection(GuiBase* parent, bool deferDelete = false);
    std::vector<GuiBase*> rawChildren(GuiBase* node);
    const std::vector<GuiBase*>* freshMembers(TreeGroup* g);   // 组成员表就地引用(owner 先 revalidate)
    // locate:parentOf/rowOf 共用的下钻定位 —— 返回 {投影父亲, 行号}(-1=需扫序列/不在投影树)。
    std::pair<GuiBase*, int> locate(GuiBase* node);
    // 把 parent 名下(传递)所有"有自己账页"的组置脏:组成员表源自 parent,parent 变则组必脏。
    void markGroupPagesDirty(GuiBase* parent);
    // 自愈:枚举 node 的原始孩子与 lastLive 比对,漂移则置脏(+可选 stderr 点名)。返回是否发现漂移。
    bool healIfDrift(GuiBase* node, Projection& p, bool verbose);
    void collectReachable(GuiBase* root, PtrSet& seen);   // prune 的显式栈 DFS(含自愈)
    void walkBindings(GuiBase* root, PtrSet& seen);       // rebind 的显式栈 DFS

    // ── 引擎全局表 ───────────────────────────────────────
    // m_byParent:总账本(装过投影的父亲 → 账页)。★unordered_map:读侧每步第一站,
    //   节点式容器 rehash 不失效元素引用(revalidate 持有的 Projection& 在任何插入下安全)。
    //   ⚠ key 可能是死指针(僵尸账页),靠 discard/prune 清理,清理只值比较永不 deref。
    std::unordered_map<GuiBase*, Projection> m_byParent;
    // m_groupOwner:组 → 原父亲全局登记。写:revalidate 建组;读:freshMembers 找 owner、prune 释放孤儿组。
    std::unordered_map<GuiBase*, GuiBase*>   m_groupOwner;
    // m_roots:注册的树根,prune/rebind 的 DFS 起点。
    std::vector<GuiBase*> m_roots;
    // m_bindings:声明式绑定表。写:bindSpec/unbindSpec;读:rebind 全量消费。
    std::vector<SpecBinding> m_bindings;
    // m_bindGen:绑定表代号,每变动 +1。与页内 bindGen 比对 → 同绑定豁免强制重算。
    unsigned m_bindGen = 0;
    // m_retired:已从账本摘除、尚未 delete 的组(读侧不释放契约 C1 的载体)。
    //   写:revalidate 淘汰分支(deferDelete=true);读:collectGarbage()(唯一 delete 处)、retiredCount()。
    std::vector<TreeGroup*> m_retired;
    size_t m_retiredHighWater = 0;     // m_retired.size() 历史峰值(内存预算用,只增不减)
    size_t m_structRev = 0;            // 投影树形状版本号(契约 C9)
    // m_paranoid:漏报检测开关。默认 Debug 开(抓漏通知)、Release 关(省枚举)。
#ifdef _DEBUG
    bool m_paranoid = true;
#else
    bool m_paranoid = false;
#endif
    // 注:刻意没有全局"成员→组"反查表(多层会互相覆盖),parentOf 从数据侧自然父亲逐层下钻,天然 deepest-wins。
};

// ── 便捷规则工厂 ──────────────────────────────────────
namespace GroupRules {

// 投影函数版:key 空 → 不分组留直属。★组节点默认【不进桶】(防 TreeGroup 套娃);
//   多层分组里"组作为成员"必须手写规则显式收录组的 label(见 PROJECT.md §1.4)。
inline GroupRule byKey(std::function<std::string(GuiBase*)> proj,
                       std::function<std::string(const std::string&)> labeler = {})
{
    return [proj, labeler](GuiBase* n) -> std::optional<GroupTicket> {
        if (!n || dynamic_cast<TreeGroup*>(n)) return std::nullopt;
        std::string k = proj(n);
        if (k.empty()) return std::nullopt;
        GroupTicket t; t.key = k; t.label = labeler ? labeler(k) : k;
        return t;
    };
}

// 谓词二分版:命中进组,不命中留直属(组节点同样默认豁免)。
inline GroupRule byPredicate(std::function<bool(GuiBase*)> pred, std::string label, int order = 0)
{
    return [pred, label, order](GuiBase* n) -> std::optional<GroupTicket> {
        if (!n || dynamic_cast<TreeGroup*>(n) || !pred(n)) return std::nullopt;
        GroupTicket t; t.key = label; t.label = label; t.order = order;
        return t;
    };
}

} // namespace GroupRules
