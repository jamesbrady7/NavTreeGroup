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
//      · 缓存检查从三十行机械塌缩成一行 `if (page.valid && !page.dirty) return;`;
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
    /**
     * @brief 取得引擎单例(进程级唯一账本)
     * @return TreeProjection& 全局唯一实例
     */
    static TreeProjection& instance();

    /**
     * @brief 析构:释放全部 TreeGroup 并兜底清空待回收队列(引擎是组的唯一所有者)
     */
    ~TreeProjection();

    using Options = GroupingOptions;   // 兼容旧名

    // ══ 写侧 API ══
    /**
     * @brief 给 parent 安装完整投影(过滤+分组+排序一次声明);入口自带 prune 自愈
     * @param [in] parent 被投影的父节点(其孩子按 Spec 加工)
     * @param [in] spec 投影规格(filter/grouping/less/opt,均可空)
     * @param [in] siblings 分组静态参与名单(仅 opt.dynamicAll=false 时生效;允许"脏",内部消毒)
     * @return vector<TreeGroup*> 本页当前生效的组列表(引擎持有,调用方不得 delete)
     */
    std::vector<TreeGroup*> install(GuiBase* parent, const ProjectionSpec& spec,
                                    std::vector<GuiBase*> siblings = {});

    /**
     * @brief 兼容旧接口:只装分组(等价 install 一个只有 grouping 槽的 Spec)
     * @param [in] parent 被分组的父节点
     * @param [in] siblings 分组静态参与名单
     * @param [in] rule 分组规则(节点 → 可选票据)
     * @param [in] opt 分组选项(塌缩/动态全员)
     * @return vector<TreeGroup*> 本页当前生效的组列表
     */
    std::vector<TreeGroup*> apply(GuiBase* parent, const std::vector<GuiBase*>& siblings,
                                  GroupRule rule, Options opt = {});

    // ── 写侧三个"立即释放组"的入口(前提:调用方已保证视图坐标失效,即在 reset 括号内)──
    //   uninstall/discard/prune 走 dropProjection(...,deferDelete=false)= 当场 delete。
    //   ⚠ 读侧与 revalidate 淘汰分支【绝不】走这里(它们 deferDelete=true,见 collectGarbage 契约)。
    /**
     * @brief 拆掉 parent 的整个投影(组【立即】删除,恢复透传)
     * @param [in] parent 装过投影的父节点;无页时幂等无操作
     */
    void uninstall(GuiBase* parent);
    void ungroup(GuiBase* parent) { uninstall(parent); }   // 旧名兼容

    /**
     * @brief 数据变化后按已装 Spec 强制重算(= prune + 置脏 + 立即 revalidate,幂等收敛)
     * @param [in] parent 装过投影的父节点;无页时只剩 prune 的自愈效果
     */
    void refresh(GuiBase* parent);

    /**
     * @brief 查询 parent 是否装有投影页
     * @param [in] parent 待查询节点(允许死指针:只做 map key 值比较)
     * @return bool true = 账本里有这一页
     */
    bool hasProjection(GuiBase* parent) const;

    /**
     * @brief 全引擎存活组数(泄漏自检用)
     * @return size_t 所有账页 groups 列表的大小之和
     */
    size_t liveGroupCount() const;

    // ══ ★数据侧变更通知(v4 使用侧的唯一新义务)══════════════════
    //   契约:动完数据就调一次(幂等、O(1) 置脏、可在批量操作末尾统一调)。
    //   通知只置脏标志,不触发重算 —— 重算发生在下次读(批量变更天然合并)。
    //   漏通知不会崩,只会投影陈旧,并由 prune/paranoid 三层兜底收敛(见文件头)。
    /**
     * @brief 通知"parent 的孩子列表变了"(增/删/移动/重排序)
     * @param [in] parent 数据发生变更的父节点;无投影页时幂等无操作
     */
    void invalidate(GuiBase* parent);

    /**
     * @brief 通知"node 自身属性变了"(材质/名字等影响 filter/grouping/less 取值)
     * @param [in] node 属性变更的节点(置脏的是它【投影父亲】所在的页)
     */
    void invalidateNode(GuiBase* node);

    /**
     * @brief 删除节点的便捷组合通知 = invalidate(parent) + discard(dying)
     * @param [in] parent dying 的数据侧父亲(孩子列表将变)
     * @param [in] dying 即将被 delete 的节点。★必须在 delete dying【之前】调;
     *            级联删整棵子树时只对子树根调一次即可(内部页变僵尸,prune 清)
     */
    void notifyRemove(GuiBase* parent, GuiBase* dying);

    /**
     * @brief 全部账页置脏(大变更后不想逐个报;下次读逐页收敛)
     */
    void invalidateAll();

    // ══ 组回收:读侧绝不 delete,只有这里才真正释放(契约 C1)══════
    //   淘汰的组只进 m_retired(账本已摘除、内存仍有效),由 collectGarbage() 在
    //   【视图 persistent index 全部失效之后】(= begin/endReset 或 begin/endRemoveRows 之内)真正 delete。
    //   NavTreeModel 的 endFullReset/endRowRemoval 已自动调用 → 走这两条通道的调用方零心智负担。
    /**
     * @brief 真正释放【整个】退役队列(契约 C3)
     * @note ⚠ 只在 **reset 通道** 内安全:`endResetModel()` 作废全部 persistent index。
     *       removeRows 通道(只作废它覆盖的行范围)请用 collectGarbageSince —— 见其注释。
     */
    void collectGarbage() override;

    /**
     * @brief 只释放 waterMark 之后入队的退役组(removeRows 通道专用;契约 C3)
     * @param [in] waterMark 窗口开始时的队列水位(= 当时的 retiredCount())
     */
    void collectGarbageSince(size_t waterMark) override;

    /**
     * @brief 待回收组数(m_retired 当前深度;长期不清会堆积,用它自检)
     * @return size_t 已淘汰但尚未 delete 的组数
     */
    size_t retiredCount() const override { return m_retired.size(); }

    /**
     * @brief 是否有待回收的组
     * @return bool m_retired 非空
     */
    bool hasRetired() const { return !m_retired.empty(); }

    // ── 结构版本号(契约 C9 失步自检;递增条件:组增删 或 flat 行数变)──
    /**
     * @brief 投影树【形状】版本号(精准操作前后各取一次,不等 = 结构被动过)
     * @return size_t 单调递增计数
     */
    size_t structRev() const override { return m_structRev; }

    // ── paranoid 漏报检测(默认 _DEBUG 开、Release 关)──────────────
    //   开:childCount 对干净页额外做"枚举 vs lastLive"比对,漂移即 stderr 点名 + 自愈。
    //   不是性能开关,是【开发期抓漏通知】的工具;Release 关掉省那一次枚举。
    /**
     * @brief 开/关漏报检测(开:childCount 对干净页额外比对,漂移即点名+自愈)
     * @param [in] on true=开启(默认 _DEBUG 开、Release 关)
     */
    void setParanoidChecks(bool on) { m_paranoid = on; }

    /**
     * @brief 查询漏报检测开关状态
     * @return bool 当前是否开启
     */
    bool paranoidChecks() const { return m_paranoid; }

    // ══ 账本内存分解(纯观测)══════════════════════════════════
    struct MemBreakdown {
        size_t pages = 0;          // 账页数(= m_byParent 条目)
        size_t specPerPage = 0;
        size_t poolNodes = 0;
        size_t lastLivePtr = 0;    // lastLive 缓存快照指针总数(自愈比对用)
        size_t flatPtr = 0;
        size_t memberGroups = 0;   // page.members 的组条目数
        size_t memberPtr = 0;      // page.members 里成员指针总数
        size_t entries = 0;        // ★ page.entries 条目总数(= flat ∪ 各组成员,单一位置表)
        size_t ownedGroupsVec = 0;
        size_t groupOwner = 0;
        size_t pageObjBytes = 0;
        size_t lastLiveBytes = 0;
        size_t flatBytes = 0;
        size_t membersBytes = 0;
        size_t entriesBytes = 0;   // ★ 单表字节(替代 v3 的 memberToGroup+memberToPos+nodePage 三张)
        size_t ownedGroupsBytes = 0;
        size_t groupOwnerBytes = 0;
        size_t specBytes = 0;
        size_t totalBytes() const {
            return pageObjBytes + specBytes + poolNodes * sizeof(void*) * 2
                 + lastLiveBytes + flatBytes + membersBytes
                 + entriesBytes + ownedGroupsBytes + groupOwnerBytes;
        }
        size_t groupObjBytes = 0;
        size_t retiredGroups = 0;
        size_t retiredHighWater = 0;
        size_t retiredBytes = 0;
        size_t totalWithRetired() const { return totalBytes() + retiredBytes; }
    };

    /**
     * @brief 账本内存分解(只读遍历,不改任何状态;内网做内存预算用它,别靠估)
     * @return MemBreakdown 条目数 + 字节估算(口径见实现注释)
     */
    MemBreakdown memoryBreakdown() const;

    /**
     * @brief m_retired 的历史峰值(只增不减;峰值出现在换规则那一瞬,事后采样采不到)
     * @return size_t 待回收队列曾经达到的最大深度
     */
    size_t retiredHighWater() const { return m_retiredHighWater; }

    /**
     * @brief 重置峰值计数(基准测试用:换一个场景前调一下,免得读到上一场景的值)
     */
    void resetRetiredHighWater() { m_retiredHighWater = m_retired.size(); }

    /**
     * @brief 组入队待回收 + 更新峰值水位。★所有 m_retired 写入必须走它
     *        (直接 push_back 会漏记高水位 → 内存基准失真)
     * @param [in] group 已从账本摘除、等待安全点释放的组
     */
    void noteRetired(TreeGroup* group) {
        m_retired.push_back(group);
        if (m_retired.size() > m_retiredHighWater) {
            m_retiredHighWater = m_retired.size();
        }
    }

    // ══ 生命周期卫生(三政体生存法则)══
    /**
     * @brief 腿①显式讣告:节点死前清掉它的账页(纯 map 操作,全程不解引用 dying)
     * @param [in] dying 即将 delete 的节点(可能正在析构栈上,虚表已退化 → 只当 key 用)
     */
    void discard(GuiBase* dying);

    /**
     * @brief 注册树根(prune/rebind 的 DFS 起点;重复注册幂等)
     * @param [in] root 树根节点(nullptr 忽略)
     */
    void registerRoot(GuiBase* root);

    /**
     * @brief 注销树根
     * @param [in] root 之前注册过的树根
     */
    void unregisterRoot(GuiBase* root);

    /**
     * @brief 腿②可达性剪枝 + 漏通知自愈:从注册根沿活枚举 DFS,
     *        走不到的账页 key = 死者 → 清条目 + 删孤儿组;沿途对每页做漂移检测
     */
    void prune();

    // ══ 腿③:声明式 Spec 绑定(跨全量重建重生)══
    struct SpecBinding {
        std::string id;                        // 绑定名(幂等去重/解绑)
        std::function<bool(GuiBase*)> locate;  // 定位谓词:命中谁就给谁装 Spec
        ProjectionSpec spec;
    };

    /**
     * @brief 注册声明式绑定并立即生效(同 id 覆盖;内部执行 rebind)
     * @param [in] binding id + 定位谓词 + Spec(locate 为空则忽略)
     */
    void bindSpec(SpecBinding binding);

    /**
     * @brief 解绑(已装投影保留现场;下次重建后不再重生。要立即拆用 uninstall)
     * @param [in] id 绑定名
     */
    void unbindSpec(const std::string& id);

    /**
     * @brief ★全量重建后必调:prune(含自愈)+ 从根 DFS 按谓词重装全部绑定
     */
    void rebind();

    // ══ 读侧 API(ITreeSource 实现,模型唯一入口)══
    //   v4:干净页零枚举直接答缓存;脏页重算一次。跨调用不再 O(k²)。
    /**
     * @brief 投影后的孩子数(唯一缓存校验点;脏页在这里触发重算)
     * @param [in] node 被查询节点(有页答 flat 行数;组答成员数;透传答原始孩子数)
     * @return size_t 孩子数;node 为空或组已被连锁淘汰时返回 0
     */
    size_t childCount(GuiBase* node) override;

    /**
     * @brief 投影序列第 i 个孩子(约定:模型先 childCount 校验,再 child 读缓存)
     * @param [in] node 被查询节点
     * @param [in] i 投影序列下标
     * @return GuiBase* 第 i 个;越界/空节点返回 nullptr
     */
    GuiBase* child(GuiBase* node, size_t i) override;

    /**
     * @brief 投影父亲(可能是组;逐层下钻 deepest-wins,走 entries 单表)
     * @param [in] node 被定位节点
     * @return GuiBase* 投影父亲;node 为空返回 nullptr
     */
    GuiBase* parentOf(GuiBase* node) override;

    /**
     * @brief 在投影父亲孩子序列里的行号
     * @param [in] node 被定位节点
     * @return int 行号;-1 = 不在投影树上(被过滤/已死)
     */
    int rowOf(GuiBase* node) override;

    /**
     * @brief 判断节点是否为展示组(TreeGroup)
     * @param [in] node 待判断节点
     * @return bool true = 组节点
     */
    bool isGroup(GuiBase* node) const override;

private:
    TreeProjection() = default;
    TreeProjection(const TreeProjection&) = delete;
    TreeProjection& operator=(const TreeProjection&) = delete;

    // 一个父亲的完整投影状态(账本的一页)。
    //
    // ★★ 读这一页之前,先分清【三个互相正交的关系】—— 它们各占各的字段,
    //    混在一起正是这个结构最难读的地方:
    //      位置(position)   : 它在本页的第几行?      → flat / entries[].row
    //      容器(containment): 它显示在谁的肚子里?     → entries[].container
    //      归属(ownership)   : 它的账页归本页管吗?     → ownedGroups
    //    ⚠ 最容易踩的误解:ownedGroups **不是**"本层顶层摆着的组"(那是 flat 里的组),
    //      而是"本页【拥有】的全部组",**含嵌在别的组肚子里的那些** ——
    //      后者不在 flat 里(它们没有自己的行,只出现在收养者的成员表里),
    //      但它们的账页仍然归本页管。
    //      凡是"级联拆页 / 传递置脏 / 记账"这类操作,问的都是【归属】不是位置,
    //      所以它们遍历 ownedGroups;而"这页顶层显示了几行"要从 flat 里挑组。
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
        // ownedGroups:本页【拥有】的全部组(引擎是唯一所有者)。
        //   构成 = (flat 里出现的组) ∪ (members 的键,即"活在别的组肚子里的嵌套组")。
        //   ⚠ 别把它当成"本层顶层组"(那是 flat 里的组),也别当"这页显示了几行" ——
        //     它与 flat【不平行】:其中的嵌套组一行都没有。
        //   为什么必须含嵌套组:下面的消费者问的都是"本页名下有哪些组":
        //     · dropProjection   —— 拆本页时要【级联】拆掉每个组自己的账页;
        //       漏掉嵌套组 = 它的页泄漏 + m_groupOwner 残留。
        //     · markGroupPagesDirty / revalidate 末尾的保守传播 —— 组成员表源自本页,
        //       本页重算后名下组页必须连坐置脏。
        //     · liveGroupCount —— 记账(测试的"退出时活组=0"就是靠它)。
        //     · revalidate 取旧组做复用/淘汰的输入(union members 键,见那里的注释)。
        //   写:revalidate 末尾;读:以上四处。
        std::vector<TreeGroup*> ownedGroups;
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

    /**
     * @brief 账页唯一创建点:取(或建)parent 的账页引用
     * @param [in] parent 页的持有者节点
     * @return Projection& 账页引用(unordered_map 节点式,引用在任何插入下稳定)
     */
    Projection& projection(GuiBase* parent) { return m_byParent[parent]; }

    /**
     * @brief 安装的内部实现(不含 prune;供 install 与 walkBindings 复用)
     * @param [in] parent 被投影的父节点
     * @param [in] spec 投影规格
     * @param [in] siblings 静态参与名单(消毒后写入 pool)
     * @return vector<TreeGroup*> 重算后的组列表
     */
    std::vector<TreeGroup*> installInternal(GuiBase* parent, const ProjectionSpec& spec,
                                            const std::vector<GuiBase*>& siblings);

    /**
     * @brief walkBindings 的安装入口:同绑定(bindId+bindGen 未变)→ 跳过重装
     * @param [in] node 谓词命中的节点
     * @param [in] binding 命中的绑定(取它的 id 与 spec)
     */
    void installFromBinding(GuiBase* node, const SpecBinding& binding);

    /**
     * @brief 对分组按投影进行重映射(脏才重算:过滤→分组→排序→建 entries)
     * @param [in] GuiBase* 被分组的父节点(其子结点会按规则被分组)
     * @param [in/out] Projection& 对应于输入父节点的分组投影信息
     */
    void revalidate(GuiBase* parent, Projection& page);

    /**
     * @brief 拆掉 parent 的账页:级联拆组名下内层页,组按 deferDelete 立即删或入待回收队
     * @param [in] parent 页持有者(允许死指针:只做 key 查找)
     * @param [in] deferDelete true=组进 m_retired(读侧路径,契约 C1);false=当场 delete(写侧边界)
     */
    void dropProjection(GuiBase* parent, bool deferDelete = false);

    /**
     * @brief 获取节点的原始子节点(绕过自身投影,防"组内分组"无限递归)
     * @param [in] node 待获取子结点的节点
     * @return vector<GuiBase*> 子结点列表(组=owner 成员表拷贝;普通节点=数据侧虚接口枚举)
     */
    std::vector<GuiBase*> rawChildren(GuiBase* node);

    /**
     * @brief 取组的"新鲜成员表"就地引用(先 revalidate owner 页保证成员新鲜;不拷贝)
     * @param [in] group 目标组
     * @return const vector<GuiBase*>* 成员表地址(有效期到 owner 页下次重算为止,取得即用);
     *         无 owner / owner 无页 / 组已被淘汰 → nullptr(等价旧行为:空表)
     */
    const std::vector<GuiBase*>* freshMembers(TreeGroup* group);

    /**
     * @brief parentOf/rowOf 共用的下钻定位(走 entries 单表,deepest-wins)
     * @param [in] node 被定位节点
     * @return pair<GuiBase*,int> {投影父亲, 行号};行号 -1 = 需调用方兜底扫描(透传父)
     *         或不在投影树(被过滤/已死)
     */
    std::pair<GuiBase*, int> locate(GuiBase* node);

    /**
     * @brief 把 parent 名下(传递)所有"有自己账页"的组置脏
     *        (组成员表源自 parent 页,parent 变则组页输入必变 → 连坐)
     * @param [in] parent 刚被 invalidate 的页持有者
     */
    void markGroupPagesDirty(GuiBase* parent);

    /**
     * @brief 漏通知自愈:枚举 node 原始孩子与 lastLive 比对,漂移则置脏(+可选点名)
     * @param [in] node 账页持有者
     * @param [in/out] page 其账页(漂移时 dirty 置位)
     * @param [in] verbose true = 漂移时向 stderr 点名(paranoid 用);false = 静默自愈(prune 用)
     * @return bool 是否发现漂移
     */
    bool healIfDrift(GuiBase* node, Projection& page, bool verbose);

    /**
     * @brief prune 的显式栈 DFS:收集从 root 可达的全部节点(含账页自愈);
     *        只沿活枚举下行,死者走不到也绝不 deref
     * @param [in] root DFS 起点
     * @param [in/out] seen 已访问指针集(只做值哈希,永不 deref)
     */
    void collectReachable(GuiBase* root, PtrSet& seen);

    /**
     * @brief rebind 的显式栈 DFS:每个活数据节点跑全部绑定谓词,命中即安装
     * @param [in] root DFS 起点
     * @param [in/out] seen 已访问指针集
     */
    void walkBindings(GuiBase* root, PtrSet& seen);

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
/**
 * @brief 构造"按投影函数取 key"的分组规则(空 key / 组节点 → 不分组)
 * @param [in] proj 节点 → key 字符串(空串 = 留直属)
 * @param [in] labeler 可选:key → 显示名(缺省 label = key)
 * @return GroupRule 可直接装进 ProjectionSpec.grouping 的规则
 */
inline GroupRule byKey(std::function<std::string(GuiBase*)> proj,
                       std::function<std::string(const std::string&)> labeler = {})
{
    return [proj, labeler](GuiBase* node) -> std::optional<GroupTicket> {
        if (!node || dynamic_cast<TreeGroup*>(node)) {
            return std::nullopt;
        }
        std::string key = proj(node);
        if (key.empty()) {
            return std::nullopt;
        }
        GroupTicket ticket;
        ticket.key = key;
        ticket.label = labeler ? labeler(key) : key;
        return ticket;
    };
}

// 谓词二分版:命中进组,不命中留直属(组节点同样默认豁免)。
/**
 * @brief 构造"谓词命中即进组"的二分规则
 * @param [in] pred 命中谓词(true = 进组)
 * @param [in] label 组名(同时作为 key)
 * @param [in] order 组间排序提示
 * @return GroupRule 可直接装进 ProjectionSpec.grouping 的规则
 */
inline GroupRule byPredicate(std::function<bool(GuiBase*)> pred, std::string label, int order = 0)
{
    return [pred, label, order](GuiBase* node) -> std::optional<GroupTicket> {
        if (!node || dynamic_cast<TreeGroup*>(node) || !pred(node)) {
            return std::nullopt;
        }
        GroupTicket ticket;
        ticket.key = label;
        ticket.label = label;
        ticket.order = order;
        return ticket;
    };
}

} // namespace GroupRules
