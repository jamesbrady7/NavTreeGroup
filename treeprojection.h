#pragma once
// ═══════════════════════════════════════════════════════════════
//  TreeProjection:展示树投影引擎(v3,原 GroupingEngine 升级更名)
// ═══════════════════════════════════════════════════════════════
//  身份:数据侧原始枚举 → 展示树的【唯一加工点】。分组只是流水线的一节,
//  过滤/排序与它平级。模型只通过 ITreeSource 四件套读投影结果。
//
//  设计根基(与 v2 一致,详见 docs/设计精讲.md):
//   · 组不存成员:成员关系 = 规则 × 父亲当前活列表,现场重算(惰性投影);
//   · 对现有对象零写入(连 m_parentForTree 都不碰);
//   · 缓存两级:raw 活列表没变 → 整条流水线都不跑;
//   · 三政体生存:discard 讣告 / registerRoot+prune 可达性剪枝 /
//     bindSpec+rebind 声明式规则跨全量重建自动重生。
//
//  流水线语义(v3 新增):
//   live = rawChildren(parent)          原始事实(数据侧现场重算)
//   → [过滤] spec.filter 为 false 的节点移出投影(被滤节点 rowOf=-1)
//   → [分组] spec.grouping 按 key 归堆,组建在首成员原位置(复用旧组保状态)
//   → [排序] spec.less 对"组+直属"混合列表 stable_sort(组内成员同比较器)
//   = flat(模型看到的投影序列)
//  顺序有语义:过滤在分组前(隐藏件不占组成员位),排序在分组后(一次比较器
//  同时管组间与直属顺序)。less 必须处理 TreeGroup(通常按 ticket.order/label),
//  且必须是严格弱序 + 有确定性 tie-breaker,否则投影抖动。
#include "guibase.h"
#include "itreesource.h"
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>
#ifdef _DEBUG
#include <thread>          // 仅 Debug 下做跨线程检测(见 onOutermostEntry)
#endif
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
    // ── 成员标注 ──────────────────────────────────────────
    // m_ticket:组的身份三件套(key=归组依据/label=显示名/order=组间排序提示)。
    //   写:构造时;revalidate 复用旧组时按 key 匹配 —— key 相同则复用同一
    //   TreeGroup 指针,QModelIndex/展开/选中状态因此跨刷新存活。
    //   读:模型 labeler(显示 label)、less 比较器(消费 order/label)。
    GroupTicket m_ticket;
    // m_owner:原父亲(组的挂载点),非持有、不管理其生死。
    //   读:parentForTree() 直接返回它(所以 parentOf(组)==owner);
    //       rawChildren(组) 靠它找到外层 owner 的投影来解析成员列表。
    //   ⚠ owner 死亡后此指针变野,但引擎只把它当 map key 做值比较、
    //     永不 deref —— 这是 prune 可达性剪枝能在死者身上工作的生存前提。
    GuiBase* m_owner;
};

// ── 投影引擎 ──────────────────────────────────────────
class TreeProjection : public ITreeSource {
public:
    static TreeProjection& instance();
    ~TreeProjection();   // 释放全部 TreeGroup(引擎是组的唯一所有者)

    using Options = GroupingOptions;   // 兼容旧名

    // ══ 写侧 API ══
    // 完整投影安装(过滤+分组+排序一次声明)。siblings 仅用于分组参与名单
    // (opt.dynamicAll=true 时忽略);允许"脏",消毒规则同 v2。
    std::vector<TreeGroup*> install(GuiBase* parent, const ProjectionSpec& spec,
                                    std::vector<GuiBase*> siblings = {});
    // 兼容旧接口:只装分组
    std::vector<TreeGroup*> apply(GuiBase* parent, const std::vector<GuiBase*>& siblings,
                                  GroupRule rule, Options opt = {});
    // ── 写侧三个"立即释放组"的入口:调用前提必须看清 ─────────────
    // ★ uninstall / discard / prune 走 dropProjection(..., deferDelete=false),
    //   即【当场 delete TreeGroup】,不进 m_retired、不等 collectGarbage。
    //   这是有意的:它们都要求调用方已保证"视图坐标失效"。
    //   三者的前提逐一写清(违反 = 与坑31 同形状的 UAF):
    //     · uninstall(parent):必须在 beginFullReset..endFullReset 之内调
    //       (或视图已销毁/尚未创建)。裸调会让视图手里的组 persistent index 悬空。
    //     · discard(dying):delete dying 之前调;其名下组随 dying 一起消失,
    //       调用方本就在走"结构变更"流程(reset 或事前坐标删除)。
    //     · prune():【可能在视图仍持有 persistent index 时被调用】,因为它同时
    //       被 install/refresh 当"操作边界自愈"自动执行。因此 prune 只应该在
    //       reset 括号内发生(install/refresh 本身也是写侧操作,同理)。
    //   ⚠ 读侧(childCount/child/parentOf/rowOf)与 revalidate 的淘汰分支
    //     【绝不】走这里 —— 它们必须 deferDelete=true(见 collectGarbage 契约)。
    void uninstall(GuiBase* parent);   // 拆掉 parent 的整个投影(组【立即】删除,恢复透传)
    void ungroup(GuiBase* parent) { uninstall(parent); }   // 旧名兼容
    void refresh(GuiBase* parent);     // 数据变化后按已装 Spec 重算(幂等收敛)
    bool hasProjection(GuiBase* parent) const;
    size_t liveGroupCount() const;     // 全引擎存活组数(泄漏自检)

    // ══ 组回收:读侧绝不 delete,只有这里才真正释放 ══════════════
    // 契约(2026-10 补,外部 review 实证发现的漏洞,见 m_retired 注释):
    //   ★ 读侧(childCount/child/parentOf/rowOf)【不得】释放任何 TreeGroup。
    //     组一旦被 view 以 QPersistentModelIndex 的 internalPointer 持有,
    //     读侧 delete 它就是 UAF,而且不发任何信号 → 视图无从得知。
    //     实测:裸删成员后只问一句 childCount(root)(= Qt 的 rowCount()),
    //     组就被 delete 掉了,liveGroupCount 从 1 变 0。
    //   ★ 因此淘汰的组只进 m_retired(账本已摘除,但内存仍有效),
    //     由 collectGarbage() 在【安全点】真正 delete。
    //   ★ 安全点 = 视图的 persistent index 全部失效之后,通常就是
    //     beginResetModel / endResetModel 之内。模型层调用范式见 navtreemodel.h。
    //   ★ 不调 collectGarbage() 会堆积(retired 组泄漏),所以 retiredCount()
    //     暴露出来供自检;引擎析构会兜底释放。
    //   ⚠ 与写侧的分工(别搞混):只有 revalidate 的淘汰分支(dropProjection
    //     deferDelete=true)会把组放进 m_retired;uninstall/discard/prune 是
    //     【立即 delete】,不进队列(理由见上面写侧三个入口的前提)。
    void   collectGarbage() override;   // 真正释放已淘汰的组(必须在安全点调)
    size_t retiredCount() const { return m_retired.size(); }   // 待回收数(泄漏自检)
    bool   hasRetired() const { return !m_retired.empty(); }

    // ── 结构版本号(实现 ITreeSource::structRev)────────────────
    // ★ 这不是优化计数器,是【契约自检工具】:惰性投影下,一次纯读也能让
    //   树的形状变化(成员被数据侧删掉 → 组空 → 组蒸发 → 行数变)。
    //   模型收不到信号,所以调用方只能靠它判断"结构被动过了没有"。
    size_t   structRev() const override { return m_structRev; }

    // ══ 账本内存分解(纯观测,不改任何状态)══════════════════
    // 回答"引擎开销那 ~130 MB 花在哪" —— 这是移植到内网做内存预算时唯一
    // 需要的数字,靠猜会错。条目数用 size(),字节数按"容器平均负载"估算
    // (哈希表按桶数组 + 节点双份算,node-based 容器的 malloc 头部按 16 B 算)。
    //   ⚠ 估算口径写在各自注释里,不是精确 sizeof 累加(那会漏掉分配器开销)。
    struct MemBreakdown {
        size_t pages = 0;          // 账页数(= m_byParent 条目)
        size_t specPerPage = 0;    // 每页 spec 的估算字节(shared_ptr/字符串/函数对象)
        size_t poolNodes = 0;      // p.pool 条目总数(staticAll 才非零)
        size_t lastLivePtr = 0;    // lastLive 缓存钥匙指针总数(=被投影节点数×页数)
        size_t flatPtr = 0;        // flat 序列指针总数
        size_t memberGroups = 0;   // p.members 的组条目数
        size_t memberPtr = 0;      // p.members 里成员指针总数(=分组成员数)
        size_t memberToGroup = 0;  // memberToGroup 条目数(≈ memberPtr)
        size_t groupsVec = 0;      // p.groups 里的组指针总数
        size_t groupOwner = 0;     // m_groupOwner 条目数(=存活组数)
        size_t pageObjBytes = 0;   // m_byParent 自身(桶数组+节点+Projection 本体)
        size_t lastLiveBytes = 0;
        size_t flatBytes = 0;
        size_t membersBytes = 0;
        size_t memberToGroupBytes = 0;
        // ── 位置索引的代价(2026-10-06 落地,与"读侧 O(1)"换来)──────
        // memberToPos:每页记 (投影父亲, 行号),条目数 = 该页 flat + 各组成员;
        // nodePage:节点 → 所属页,条目数与 memberToPos 一一对应(全局一份)。
        // 两者合计约 60~70 B/被投影节点 —— 这是拿内存换掉 O(k²) 的明码标价。
        size_t memberToPos = 0;        // 条目数
        size_t nodePage = 0;           // 条目数(= memberToPos 的全局合计)
        size_t memberToPosBytes = 0;
        size_t nodePageBytes = 0;
        size_t groupsBytes = 0;
        size_t groupOwnerBytes = 0;
        size_t specBytes = 0;
        size_t totalBytes() const {
            return pageObjBytes + specBytes + poolNodes * sizeof(void*) * 2
                 + lastLiveBytes + flatBytes + membersBytes
                 + memberToGroupBytes + groupsBytes + groupOwnerBytes
                 + memberToPosBytes + nodePageBytes;
        }
        // 引擎自产内存(不含 TreeGroup 对象本体 —— 那是"展示树节点",
        // 内网若已有同名节点概念,这部分可以省掉)
        size_t groupObjBytes = 0;  // 存活组数 × sizeof(TreeGroup) 估
        // ── 延迟释放的代价(C4 的直接证据)──────────────────────────
        // retiredGroups:当前待回收(m_retired.size())
        // retiredHighWater:历史峰值 —— 【一次性 wipe 十万组时峰值吃多少内存】
        //   全靠这个才量得出来:峰值出现在重建【中途】,事后采样采不到,
        //   所以引擎自己用只增不减的计数器记高水位(PROJECT.md §5 缺口 #3)。
        size_t retiredGroups = 0;
        size_t retiredHighWater = 0;
        size_t retiredBytes = 0;       // retiredGroups × sizeof(TreeGroup) 估
        // 峰值内存合计 = 稳态账本 + 延迟释放上界(做内存预算用这个,不是 totalBytes)
        size_t totalWithRetired() const { return totalBytes() + retiredBytes; }
    };
    MemBreakdown memoryBreakdown() const;

    // m_retired 的历史峰值(只增不减)。内存预算/诊断用。
    size_t retiredHighWater() const { return m_retiredHighWater; }
    // 重置峰值计数(基准测试用:换一个场景前调一下,免得读到上一场景的值)
    void   resetRetiredHighWater() { m_retiredHighWater = m_retired.size(); }

    // 组入队待回收 + 更新峰值水位。**所有 m_retired 的写入必须走它**
    // (直接 push_back 会漏记高水位 → 内存基准失真,见 PROJECT.md §5 缺口 #3)。
    void noteRetired(TreeGroup* g) {
        m_retired.push_back(g);
        if (m_retired.size() > m_retiredHighWater) m_retiredHighWater = m_retired.size();
    }

    // ══ 生命周期卫生(三政体生存法则,语义同 v2) ══
    void discard(GuiBase* dying);      // 腿①:显式讣告(delete 前调;纯 map 操作,不 deref 死者)
    void registerRoot(GuiBase* root);
    void unregisterRoot(GuiBase* root);
    void prune();                      // 腿②:可达性剪枝(僵尸投影清除+孤儿组释放)

    // ══ 腿③:声明式 Spec 绑定(分组意图锚定"定位谓词",跨全量重建重生) ══
    struct SpecBinding {
        std::string id;                        // 绑定名(幂等去重/解绑)
        std::function<bool(GuiBase*)> locate;  // 定位谓词:命中谁就给谁装 Spec
        ProjectionSpec spec;
    };
    void bindSpec(SpecBinding b);              // 注册并立即生效
    void unbindSpec(const std::string& id);    // 解绑(已装投影保留;要立即拆用 uninstall)
    void rebind();                             // ★全量重建后必调:prune + 按谓词重装全部绑定

    // ══ 读侧 API(ITreeSource 实现,模型唯一入口) ══
    // ★ 这四个是"查询纪元"的边界:每个都自增 m_queryEpoch(见下面 beginQuery)。
    //   纪元保证"一次操作内同一账页只校验一次",而跨操作必重新枚举活列表。
    size_t   childCount(GuiBase* node) override;   // 唯一缓存校验点
    GuiBase* child(GuiBase* node, size_t i) override;
    GuiBase* parentOf(GuiBase* node) override;     // 逐层下钻,deepest-wins
    int      rowOf(GuiBase* node) override;
    bool     isGroup(GuiBase* node) const override;

    // ══ ★纪元优化的逃生阀(契约 C5 的正式开关)════════════════════
    //   背景:查询纪元(EpochScope + checkedAt)的正确性依赖"单线程 + 一次
    //   操作内数据侧不变"(见 m_queryEpoch 注释)。这是【设计约束,不是假设】——
    //   但原来它只写在注释里,没有开关,一旦前提不成立就只能改源码。
    //   现在的处置:
    //     · 前提成立(默认)→ 纪元生效,行为与性能都最优。
    //     · 前提不成立(你们的模型跨线程/异步改数据树)→ 调下面这个开关,
    //       引擎退回"每次操作都重新枚举活列表"。**行为完全不变,只是慢一点**,
    //       而且不崩溃(最坏是投影陈旧,会在下一次操作被枚举纠正)。
    //   ⚠ 它不是性能开关,别为了提速去关:关掉约等于放弃"同一操作内只校验一次"。
    void   setEpochOptimization(bool enabled);
    bool   epochOptimizationEnabled() const { return m_epochOptimOn; }
    // 跨线程误用的诊断:返回"检测到过的跨线程进入次数"(正常应恒为 0)。
    //   只在 Debug CRT 下检测线程身份;Release 下恒为 0(不是"没问题"的证据)。
    size_t crossThreadEntryCount() const { return m_crossThreadEntries; }

private:
    // 开启一次"查询纪元"。放在每个公开操作入口第一行(RAII,析构自动回收)。
    //   纪元只用于 revalidate 第 0 步的跳过判定,不参与任何业务判定 ——
    //   把它删掉(恒不递增)引擎行为完全不变,只是慢一点(见 m_queryEpoch 注释)。
    // ★ 只有【最外层】入口才递增:rowOf 内部会调 parentOf,二者必须共享同一纪元,
    //   否则内层递增会把外层已盖的 checkedAt 作废(退化成每次都重校验,白付代价)。
    class EpochScope {
    public:
        explicit EpochScope(TreeProjection* p) : m_p(p) {
            if (m_p->m_epochDepth++ != 0) return;        // 嵌套:不重开纪元
            ++m_p->m_queryEpoch;
            m_p->onOutermostEntry();                      // 跨线程检测(Debug 有效)
        }
        ~EpochScope() { --m_p->m_epochDepth; }
        EpochScope(const EpochScope&) = delete;
        EpochScope& operator=(const EpochScope&) = delete;
    private:
        TreeProjection* m_p;
    };
    friend class EpochScope;
    // 最外层入口进入时的跨线程检测。只在 _DEBUG 下真正比对线程身份
    // (Release 下只自增计数、不比对 —— 诚实起见,Release 的 0 不代表没问题)。
    void onOutermostEntry();
    TreeProjection() = default;
    TreeProjection(const TreeProjection&) = delete;
    TreeProjection& operator=(const TreeProjection&) = delete;

    // 一个父亲的完整投影状态(账本的一页)。
    // ── 成员标注(写时机 → 读时机)──────────────────────────
    struct Projection {
        // spec:本页的投影定义(filter/grouping/less/opt 四槽,均可空)。
        //   写:install/bindSpec;读:revalidate 每次重算消费三个槽,
        //   refresh 就是"拿旧 spec 重跑一遍"。
        ProjectionSpec spec;
        // pool:分组静态参与名单 —— 仅 dynamicAll=false 时生效,
        //   只有名单内的节点可能被拉进组。
        //   写:install(siblings) 消毒后(去空/去重/剔外来组/展开本父亲的旧组);
        //   读:revalidate 分组阶段做 vis ∩ pool 交集。dynamicAll=true 时忽略。
        std::set<GuiBase*> pool;
        // ── 缓存区(childCount 时校验,变了才重算)──
        // lastLive:上次校验时 rawChildren 的原始活列表快照(缓存钥匙)。
        //   写/读:revalidate 第 0 步拉当前活列表与它做指针序列比对,
        //   完全一致 → 整条流水线跳过。"数据侧删节点引擎自动发现"就靠它。
        std::vector<GuiBase*> lastLive;
        // flat:流水线最终产物 —— 过滤→分组→排序后的投影序列(组+直属混排,
        //   组放在首成员原位置)。写:revalidate 末尾;
        //   读:childCount/child/rowOf 全部读它 —— 模型行号的唯一来源。
        std::vector<GuiBase*> flat;
        // groups:当前生效的组列表(引擎是这些 TreeGroup 的唯一所有者)。
        //   写:revalidate 重建;未被复用的旧组在此被 cull
        //   (dropProjection+delete,级联拆组名下的内层投影);
        //   读:uninstall/引擎析构靠它删干净所有组。
        std::vector<TreeGroup*> groups;
        // members:组 → 活成员列表(已按 less 排序)。写:revalidate;
        //   读:rawChildren(TreeGroup) —— 这是组成员关系的【唯一存储处】,
        //   惰性重算、随活列表刷新,组对象本身零成员指针。
        std::map<TreeGroup*, std::vector<GuiBase*>> members;
        // memberToGroup:成员 → 所属组(刻意只存【本层局部】,无全局版 ——
        //   多层分组下全局 map 会内外层互相覆盖,v2 实测踩坑)。
        //   写:revalidate;读:parentOf 逐层下钻 —— 自然父亲有投影时查本层,
        //   命中 → 答案是组,不命中 → 答案是自然父亲(deepest-wins)。
        //   ★ unordered_map:parentOf 每层下钻都查它,宽亲页面动辄数百成员,
        //     红黑树的 log 查找在读侧热路径上纯浪费。只按值查找,无序无影响。
        std::unordered_map<GuiBase*, TreeGroup*> memberToGroup;
        // memberToPos:节点 → (投影父亲, 在该父亲孩子序列里的行号)。
        //   ★ 为什么需要它(2026-10-06 性能定位的结论):
        //     模型对每一行都会问 parent()/rowOf(),而旧实现为了给出答案必须
        //     "枚举该页的全部活孩子"(revalidate 的缓存校验)—— 于是遍历 k 行
        //     要做 k 次 O(k) 枚举 ⇒ 整棵树 O(k²)。实测:单父亲 5000 个孩子时,
        //     一次展开首屏要 76 ms;2000 个孩子时一次全树遍历 11.6 秒。
        //   ★ 行号【不能】用"自然父亲页里的 flat 位置"算:被分组的成员根本不在
        //     自然父亲的 flat 里(那里放的是组),它的行号是"在所在组的孩子序列里
        //     的位置"。所以这一项必须成对存下 (投影父亲, 行号)。
        //   ★ 建表时机:revalidate 末尾、宣布有效之前 —— 一次 O(总孩子数)。
        //     查询期只查表,不再需要 revalidate ⇒ parentOf/rowOf 都是 O(1)。
        //   写:revalidate;读:parentOf/rowOf 的快路径(查不到则回退旧路径)。
        std::unordered_map<GuiBase*, std::pair<GuiBase*, int>> memberToPos;
        // valid:账页是否已算过。revalidate 置位;child() 读 flat 前检查。
        bool valid = false;
        // checkedAt:本页最后一次【通过活列表校验】时的查询纪元(见 m_queryEpoch)。
        //   写:revalidate 每次真正校验完(含命中缓存与跑完整条流水线两种)置位;
        //   读:revalidate 第 0 步先比它 —— 同一纪元内已校验过就直接返回。
        //   ★ 解决的是"一次查询里同一页被校验多次":parentOf 要下钻查
        //     memberToGroup,rowOf 随后又要扫 flat,两次都调 revalidate;
        //     组页更甚 —— 组 revalidate 内部还要回头校验 owner。实测
        //     parentOf+rowOf 一次查询重复校验 2~3 次同一 100 孩子账页。
        unsigned long long checkedAt = 0;
        // ── 绑定出身证明(walkBindings 的"同绑定豁免"用)──
        // bindId:装这页时用的 SpecBinding id;手动 install 装的页为空。
        // bindGen:装这页时引擎的绑定代号(m_bindings 每变动一次 +1)。
        //   rebind 走全树时:id 与代号都没变 → Spec 必然一字未动 →
        //   跳过"强制 valid=false 全量重算",只做缓存校验(命中≈零成本)。
        //   没有这两项时,rebind 会让 prune 刚算完的 1 万页全部再算一遍
        //   (bench 实测 walk 从 ~0.5s 膨胀到 ~1.1s)。
        std::string bindId;
        unsigned bindGen = ~0u;
    };

    // ★ 账页的【唯一创建点】——提示位图的置位义务全压在这一行(见 m_pageHint)。
    //   任何将来新增的建页路径都必须走这里,否则会漏置位 → 活页被误当僵尸删。
    Projection& projection(GuiBase* parent) {
        // ★ 这一行不能省:prune 的 DFS 用 m_pageHint 门控"你有没有账页"的探测
        //   (hintProbe(m_pageHint, n) 为 false ⇒ 直接当普通节点走,不查 m_byParent)。
        //   漏置位 = 活页被当成僵尸删掉 —— 实测:去掉本行,interactive_demo 的
        //   M(嵌套 + 重建)立刻 FAIL。所以建页与置位必须成对,建页点唯一 = 这里。
        hintMark(m_pageHint, parent);
        return m_byParent[parent];
    }
    std::vector<TreeGroup*> installInternal(GuiBase* parent, const ProjectionSpec& spec,
                                            const std::vector<GuiBase*>& siblings);
    void installFromBinding(GuiBase* n, const SpecBinding& b);   // walkBindings 用:同绑定豁免
    void revalidate(GuiBase* parent, Projection& p);   // 流水线:过滤→分组→排序
    //   deferDelete=true:只把组丢进 m_retired 而不 delete(读侧路径用)。
    //     读侧释放 UAF 的完整说明见 m_retired。写侧(uninstall/discard/prune)
    //     传 false = 立即 delete(调用方已保证坐标失效)。
    void dropProjection(GuiBase* parent, bool deferDelete = false);
    std::vector<GuiBase*> rawChildren(GuiBase* node);  // 原始枚举(绕过自身投影,防组内分组递归)
    // 组的"新鲜成员表"就地引用(owner 页先 revalidate;无 owner/无此组 → nullptr)。
    // ★ 返回指针而非拷贝向量:childCount/child/DFS 对组的高频访问从
    //   "每次拷整张成员表"降为 O(1)。指针只在下次 owner 重算前有效,立即使用。
    const std::vector<GuiBase*>* freshMembers(TreeGroup* g);
    // 缓存钥匙就地比对:不建向量,逐位比 rawChildren 序列与 lastLive。
    // 命中(childCount 热路径的常态)零分配;未命中才走 rawChildren 建快照。
    bool liveMatchesLast(GuiBase* node, Projection& p);
    // 提示位图:probe(测试)+ mark(置位,永不清除)。见 m_pageHint 的注释。
    //   ★ 定长、永不扩容 —— 这是本实现能"只增不删"的前提。若允许按需扩容,
    //     哈希值接近 2^64 时容量计算会溢出成天文数字(实测直接 length_error
    //     → abort,已踩),且扩容期间的重建本身又是一次全量开销。
    //   误报率:2^23 位 = 838 万槽,10 万组 + 1 万页时约 1.3%。误报只是
    //     白做一次哈希探测,无害;真正要命的漏报被"创建点唯一"堵死。
    static const size_t HINT_BITS_LOG2 = 23;
    static size_t hintIndex(const void* p) {
        uint64_t x = (uint64_t)(uintptr_t)p;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return (size_t)x & ((size_t(1) << HINT_BITS_LOG2) - 1);   // ★ 先截断再定位
    }
    static bool hintProbe(const std::vector<uint64_t>& bits, const void* p) {
        if (bits.empty()) return false;
        size_t i = hintIndex(p);
        return (bits[i >> 6] >> (i & 63)) & 1u;
    }
    static void hintMark(std::vector<uint64_t>& bits, const void* p) {
        if (bits.empty()) bits.assign(size_t(1) << (HINT_BITS_LOG2 - 6), 0);  // 1 MB
        size_t i = hintIndex(p);
        bits[i >> 6] |= (uint64_t)1 << (i & 63);
    }
    // prune/rebind 的 DFS —— ★ 显式栈迭代版(递归版在 10 万级深链上
    //   实测爆栈崩溃;引擎自身不能有递归深度上限,树多深都得能走)
    void collectReachable(GuiBase* root, PtrSet& seen);
    void walkBindings(GuiBase* root, PtrSet& seen);

    // ── 引擎全局四张表(成员标注)───────────────────────────
    // m_byParent:总账本,装过投影的父亲 → 它的账页。
    //   写:install/bindSpec(rebind 时经谓词命中);读:一切查询的第一站 ——
    //   childCount(node) 先查它(有账页=优先看自身投影,解决"组里再分组"
    //   的优先级坑)、parentOf 下钻逐层查、hasProjection 就是查它。
    //   ⚠ key 可能是死指针(僵尸账页/地址复用幽灵),靠 discard/prune 清理;
    //     清理时只做值比较,永不 deref。
    //   ★ 用 unordered_map 而非 map:这是读侧四件套每一步的第一站(parentOf 下钻
    //     每层一次、rowOf 一次、childCount 一次)。bench 实测 10k 键规模随机
    //     查找:std::map 95 ns/次 vs unordered_map 16 ns/次(6 倍),因为红黑树的
    //     每级都是一次可能 cache miss 的指针追逐。
    //   ★ 换 unordered_map 不破坏任何东西:(a) 引用/指针稳定性 —— 标准保证
    //     rehash 只失效迭代器,【不失效元素的引用与指针】(节点式容器),所以
    //     revalidate 持有的 Projection& 在任何插入下都安全;
    //     (b) 全库对 m_byParent 的遍历只有 4 处,全是顺序无关的
    //     (析构清账 / liveGroupCount 求和 / prune 收 stale / hasProjection),
    //     没有一处依赖有序。
    std::unordered_map<GuiBase*, Projection> m_byParent;
    // m_groupOwner:组 → 原父亲的全局登记。
    //   写:revalidate 建组时;读:rawChildren(TreeGroup) 找 owner
    //   (组不知道自己挂在哪本账上)、prune 发现 owner 不可达时释放孤儿组。
    //   ★ 同样改 unordered_map:本表在宽场景下有【十万级】条目(每个组一条),
    //     而 freshMembers 在 childCount/child/两个 DFS/rowOf 里都是热路径 ——
    //     std::map 的 17 级指针追逐实测是 parentOf+rowOf 里最大的单项开销。
    std::unordered_map<GuiBase*, GuiBase*>   m_groupOwner;
    // m_roots:注册的树根,prune/rebind 的 DFS 起点。
    //   写:registerRoot(建树后调一次)/unregisterRoot;
    //   读:prune 与 rebind 从根沿【活枚举】走可达集,走不到的 key 即死者。
    std::vector<GuiBase*> m_roots;
    // m_bindings:声明式绑定表(id + locate 谓词 + spec)。
    //   写:bindSpec 追加(同 id 去重)/unbindSpec 移除;
    //   读:rebind 全量消费 —— prune 后从根 DFS,每个活节点跑一遍所有
    //   locate 谓词,命中且没装 → 装。"意图锚定谓词而非指针":
    //   wipe-and-rebuild 后新节点被谓词重新命中,分组自动重生。
    std::vector<SpecBinding> m_bindings;
    // m_queryEpoch:查询纪元计数器。语义:"自上次递增以来,数据侧没有发生过变更"。
    //   写:每次【外部可感知的操作入口】(revalidate 族之外:childCount/child/
    //     parentOf/rowOf/hasProjection 以及任何写侧 API)开始时 ++;
    //   读:revalidate 第 0 步 —— 若本页 checkedAt == 当前纪元,说明它已在本纪元内
    //     校验过,直接跳过(不再枚举活列表、不再比对序列)。
    //   ★ 安全性依赖"数据侧变更必然经过引擎 API 或与引擎无交互":引擎惰性发现
    //     变更的唯一途径就是枚举活列表,而纪元只在一次操作内有效 —— 一次操作内
    //     数据侧不可能被改(单线程同步语义)。跨操作纪元必递增 → 一定重新枚举。
    //     若你们的模型【跨线程/异步】改数据树,禁用本优化(改 revalidate 里
    //     那个 if 即可退回全枚举,行为不变只慢)。
    unsigned long long m_queryEpoch = 0;
    // m_epochOptimOn:纪元优化开关(默认开)。关掉后 revalidate 第 0 步的
    //   checkedAt 短路完全失效 → 每次操作都重新枚举活列表,行为不变只慢。
    //   这是契约 C5 的正式逃生阀(见公开的 setEpochOptimization)。
    bool m_epochOptimOn = true;
    // m_crossThreadEntries:检测到的"跨线程进入引擎"次数。正常应恒为 0。
    //   ⚠ Release 下只累加、不比对线程身份,所以 Release 的 0 **不是**"没问题"
    //     的证据 —— 它只说明"没人调 crossThreadEntryCount 之外的事"。
    size_t m_crossThreadEntries = 0;
#ifdef _DEBUG
    std::thread::id m_entryThread;     // 首次进入的线程身份
    bool m_threadKnown = false;
#endif
    // m_retired:已从账本摘除、但【尚未 delete】的组 —— 读侧不释放契约的载体。
    //   写:revalidate 的淘汰分支(deferDelete=true)与 uninstall/discard/prune;
    //   读:collectGarbage()(唯一真正 delete 的地方)、retiredCount() 自检。
    //   ★ 为什么必须有它(外部 review 用探针实证):
    //     裸删成员后只调一句 P.childCount(root)(等价 Qt rowCount())就会
    //     走到 revalidate → 淘汰 → delete g,全程零信号。而 QTreeView 对每个
    //     展开的组持有 QPersistentModelIndex,其 internalPointer 正是那个
    //     TreeGroup* → 释放即 UAF,且视图完全不知情。
    //   ★ 为什么"只置不删"就够:组是我们自己 new 的,除了引擎没人会 delete 它,
    //     所以"留在原地不动"内存永远有效 —— UAF 被降级为
    //     "读到过期状态",而后者在 reset 通道内是可接受的。
    //   ★ 为什么不能顺手在 prune 里回收:prune 可能在视图仍持有
    //     persistent index 时被调用(它是"操作边界自动自愈")。回收时机
    //     必须由模型层在坐标失效之后决定。
    std::vector<TreeGroup*> m_retired;
    // m_retiredHighWater:m_retired.size() 的历史峰值(只增不减)。
    //   写:每次 m_retired.push_back 后取 max;读:retiredHighWater()、
    //       memoryBreakdown().retiredHighWater。
    //   为什么引擎要自己记:延迟释放的峰值出现在**重建中途**,事后采样采不到,
    //   而"一次性 wipe 十万组多吃多少 MB"正是内存预算必须知道的数
    //   (PROJECT.md C4 / §5 缺口 #3)。
    size_t m_retiredHighWater = 0;
    // m_structRev:投影树【形状】版本号。每次 flat 行数变化或组增删就 ++。
    //   写:revalidate 末尾;读:structRev()(ITreeSource,给模型/应用做失步自检)。
    size_t m_structRev = 0;
    // m_epochDepth:当前嵌套的 EpochScope 层数(>0 表示正处于某次操作内)。
    //   写:EpochScope 构造/析构;读:EpochScope 构造(决定要不要递增纪元)。
    int m_epochDepth = 0;

    // ── 剪枝加速:存在性提示位图(纯加速,不是权威数据)────────────
    // prune/walkBindings 要走遍全树,对【每个节点】问两个问题:
    //   "你有账页吗?"(m_byParent 探测) + "你是组吗?"(dynamic_cast)
    // 百万节点里真正有账页的约 1 万、是组的约 10 万 —— 89% 的节点两个答案
    // 都是"否",却付了哈希探测的 cache miss + dynamic_cast 的 RTTI 走链。
    //   实测这两个操作合计约 60~80 ns/节点 = 百万节点下 60~80 ms 的纯浪费。
    //
    // 提示位图把"答不上来"变成一次位测试:定长连续数组(单张 1 MB),
    //   常驻 L2/L3,代价 ~2 ns。
    //
    // ★ 失效方向是【安全的】,这是能用它的前提:
    //   位图【只置位、从不清除】。所以它可能撒谎,但只往【多算】的方向撒谎:
    //     位已置但账页已被拆  → 探测落空 → 按普通节点处理(正确,只是白测一次);
    //     位已置但地址被新对象复用且恰好不是组 → dynamic_cast 返回空 → 同上(正确);
    //   唯一危险是【漏置位】(账页真存在却没提示 → 跳过它 → 它的子树不可达 →
    //     活着的账页被当成僵尸删掉)。所以置位点收敛到两处唯一的创建点:
    //     projection() 建页、revalidate 的 new TreeGroup。审计这两个点即可。
    //   容量定长 2^23 位(1 MB/张),永不回收 —— 僵尸位让位图虚高,但位图本身
    //   极廉价,且定长换来"零扩容、零重建"。
    std::vector<uint64_t> m_pageHint;   // bit = 该地址【可能】有账页
    std::vector<uint64_t> m_groupHint;  // bit = 该地址【曾经】是个组

    // ── 位置索引(2026-10-06 性能落地)────────────────────────────    // m_nodePage:节点 → 它【出现在哪一页的孩子序列里】。
    //   为什么需要:模型对每一行都问 parent()/rowOf(),而旧的 parentOf 为了
    //   "确认账本新鲜"必须枚举该页全部孩子 —— 于是遍历 k 行 = k 次 O(k) ⇒ O(k²)。
    //   有了这张表,parentOf/rowOf 直接落到那一页查 memberToPos,全程 O(1)。
    //   ★ 写入时机:revalidate 末尾(与 memberToPos 同步,同一份数据两处记)。
    //     一个节点只会出现在【一页】的孩子序列里:
    //       自然父亲有页 → 出现在它的 flat(或它名下某个组的 members);
    //       自然父亲没页 → 不出现在任何页(所以查不到 = 不在投影树上,语义一致)。
    //     实测佐证:改造前 parentOf 的循环里 `memberToGroup.find(node)` 命中后
    //     就不再向上,正是"唯一归属页"这一性质的体现。
    //   ★ 更早的写入点(建页时)不足以覆盖:节点被分组后只在【组的孩子序列】里,
    //     所以要等 revalidate 末尾成员账本齐了再一起登记。
    //   ★ 清理:dropProjection 拆页时按该页 flat + groups 反查删除(不依赖顺序,
    //     所以先收集再删)。引擎析构时随 m_byParent 一起走。
    std::unordered_map<GuiBase*, GuiBase*> m_nodePage;


    // m_bindGen:绑定表代号,bindSpec/unbindSpec 每变动一次 +1。
    //   写:两个绑定 API;读:installFromBinding 与页内 bindGen 比对 ——
    //   表没动过且 id 相同 → 该页 Spec 必然没变 → 免强制重算(见 Projection.bindGen)。
    unsigned m_bindGen = 0;
    // 注:刻意没有全局"成员→组"反查表(多层分组会互相覆盖),
    //     parentOf 从数据侧自然父亲逐层下钻,天然 deepest-wins。
};

// ── 便捷规则工厂 ──────────────────────────────────────
namespace GroupRules {

// 投影函数版:key 空 → 不分组留直属;labeler 可选(默认 label = key)
// ★ 组节点默认【不进桶】(返回 nullopt):多层分组里"组作为成员"必须是有意的
//   (手动规则显式收录组的 label),通用规则若把组扫进桶会产生"TreeGroup 套娃"。
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

// 谓词二分版:命中进组,不命中留直属(组节点同样默认豁免,理由同 byKey)
inline GroupRule byPredicate(std::function<bool(GuiBase*)> pred, std::string label, int order = 0)
{
    return [pred, label, order](GuiBase* n) -> std::optional<GroupTicket> {
        if (!n || dynamic_cast<TreeGroup*>(n) || !pred(n)) return std::nullopt;
        GroupTicket t; t.key = label; t.label = label; t.order = order;
        return t;
    };
}

} // namespace GroupRules
