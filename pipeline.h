#pragma once
// ═══════════════════════════════════════════════════════════════
//  Pipeline:投影流水线的纯函数阶段(从 revalidate 抽出,可独立单测)
// ═══════════════════════════════════════════════════════════════
//  设计纪律:本文件所有函数【纯】—— 不碰引擎状态、不 new/delete、
//  不 deref 可疑指针;输入进、输出出,同样输入必得同样输出。
//  所有权操作(建组/删组/清账)留在引擎 revalidate 里做编排。
//
//  数据流(与 docs/设计精讲.md §8 一致):
//    live ─[filterLive]→ vis ─[bucketByKey]→ buckets ─[shouldCollapse]→
//    ─[matchReusable]→ 复用/新建/淘汰名单 ─[sortMembers]→ ─[buildFlat]→
//    flat ─[sortFlat]→ 最终投影序列
//
//  每一块的"输入→输出"实例见 tests.cpp T1~T6(可直接跑 unit_tests 看)。
#include "treeprojection.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace Pipeline {

using Less = std::function<bool(GuiBase*, GuiBase*)>;

// ── Stage A:过滤 ─────────────────────────────────────
// 职责:filter 返回 false 的节点移出投影(之后 rowOf=-1,视图上没有这行)。
// 输入:live    = 原始活列表(数据侧枚举,死者天然缺席)
//       filter = 谓词;空 = 不过滤,全员通过
// 输出:vis     = 进入投影的节点,保持 live 的相对顺序
inline std::vector<GuiBase*> filterLive(const std::vector<GuiBase*>& live,
                                        const std::function<bool(GuiBase*)>& filter) {
    std::vector<GuiBase*> vis;              // 输出:通过过滤的节点(visible),保持 live 的原顺序
    vis.reserve(live.size());               // 最多 live.size() 个,一次分配到位
    for (GuiBase* n : live)                 // n = 当前受审的节点
        // filter 为空 → 没设过滤规则,全员放行;否则问谓词:true=留下,false=移出投影
        if (!filter || filter(n)) vis.push_back(n);
    return vis;
}

// ── Stage B-1:分桶 ───────────────────────────────────
struct Bucket {
    GroupTicket ticket;               // 桶的身份(key 相同 = 同一个组)
    std::vector<GuiBase*> members;    // 桶内成员(保持 vis 顺序)
};
// 职责:对每个节点跑 grouping 规则,按票据 key 归堆。
// 输入:vis          = 过滤后的序列
//       pool         = 静态参与名单;nullptr = dynamicAll(全员参与)
//       grouping     = 规则(节点 → 可选票据);空 = 不分组,返回空表
// 输出:buckets      = 桶列表(按各 key 首个成员的出现顺序)
//       participants = 实际参与分组的节点数(供塌缩判定;出参)
// 逃生口:规则返回 nullopt 或空 key → 不进桶(该节点留作直属)。
inline std::vector<Bucket> bucketByKey(const std::vector<GuiBase*>& vis,
                                       const std::set<GuiBase*>* pool,
                                       const GroupRule& grouping,
                                       size_t& participants) {
    std::vector<Bucket> buckets;            // 输出:桶列表,按各 key 首个成员的出现顺序排
    std::unordered_map<std::string, size_t> bucketOf;   // key → 桶序号(找桶 O(1),替代线性 find_if)
    participants = 0;                       // 出参清零:有资格且真正被规则审过的节点数
    if (!grouping) return buckets;          // 没设分组规则 → 空桶表(全员留直属)
    for (GuiBase* node : vis) {             // node = 当前受审节点(可能含"旧组",见 revalidate 的 parts)
        // 第一道闸【资格】:pool 非空(静态白名单模式)且 node 不在名单上 → 跳过,
        // 连规则都不问,永远直属。pool==nullptr 即 dynamicAll,人人有资格。
        if (pool && !pool->count(node)) continue;
        // 注:组节点【允许】进桶 —— 这是多层分组("把组和其它节点一起再分一组")
        // 的入口:旧组作为整体成为新外层组的成员。revalidate 负责把旧组喂进来。
        ++participants;                     // 过了资格闸,计数(供塌缩判定用)
        // 第二道闸【去向】:问规则"你该进哪个组" → 票据(含 key/label/order)
        auto t = grouping(node);            // t 的类型是 std::optional<GroupTicket>
        // 逃生口:规则说"不分组"(nullopt)或给了空 key → 留直属
        if (!t || t->key.empty()) continue;
        // 在已有桶里找同 key 的桶(key = 组的身份证号,同 key = 同一个组)。
        // ★ 用 bucketOf 哈希索引 O(1) 定位(曾是 find_if 线性找桶:
        //   宽父亲 20 万孩子 × 100 桶实测退化成 O(孩子×桶) 字符串比较)
        auto it = bucketOf.find(t->key);
        if (it == bucketOf.end()) {
            // 没找到 → 这个 key 第一次出现,开新桶:桶身份=票据,首个成员=node
            bucketOf.emplace(t->key, buckets.size());   // 记下 key → 桶序号
            Bucket b; b.ticket = *t; b.members.push_back(node);
            buckets.push_back(std::move(b));
        } else {
            buckets[it->second].members.push_back(node); // 归入已有桶(保持 vis 顺序)
        }
    }
    return buckets;
}

// ── Stage B-2:单组塌缩判定 ───────────────────────────
// 职责:全员同组时不建废层级(root 下只有一个组 = 白多点一次展开)。
// 输入:buckets/participants/visCount/opt
// 输出:true = 应清空 buckets(三个条件缺一不可:开了开关、恰好一桶、
//       桶内成员数 == 参与数 == vis 总数,即没有任何直属残余)
inline bool shouldCollapse(const std::vector<Bucket>& buckets, size_t participants,
                           size_t visCount, const GroupingOptions& opt) {
    // 四个条件同时成立才塌缩,缺一不可:
    // ① opt.collapseIfSingleGroup          开关打开了(调用方想要这个行为)
    // ② buckets.size() == 1                恰好只有一个桶(多桶没得塌)
    // ③ buckets[0].members.size()==participants
    //                                       参与分组的节点全都进了这个桶(没人走逃生口)
    // ④ participants == visCount            可见节点全都参与了分组(没有直属残余)
    // ③④合起来 = "全员同组":此时 root 下只剩一个组,多点一次展开纯属浪费 → 不建组
    return opt.collapseIfSingleGroup && buckets.size() == 1
        && buckets[0].members.size() == participants && participants == visCount;
}

// ── Stage B-3:组复用匹配(纯匹配,不做任何所有权操作)──
// 职责:按 key 把旧组匹配给新桶 —— 同 key 复用旧 TreeGroup 指针
//       (QModelIndex/展开/选中状态因此跨重算存活)。
// 输入:buckets   = 新桶列表
//       oldGroups = 本页账当前生效的旧组
// 输出:返回值[i] = 第 i 桶应复用的旧组(nullptr = 需新建)
//       culled    = 未被任何桶复用的旧组(出参;交引擎 dropProjection+delete)
inline std::vector<TreeGroup*> matchReusable(const std::vector<Bucket>& buckets,
                                             const std::vector<TreeGroup*>& oldGroups,
                                             std::vector<TreeGroup*>& culled) {
    // byKey:旧组的"身份证号索引" key → 旧组指针。
    // 按 key 配对(而非位置/指针):数据怎么变,"Steel 组"这个概念是同一个。
    // (若旧组里意外有同 key 重复,后者覆盖前者,前者自然落入 culled 被清掉)
    std::map<std::string, TreeGroup*> byKey;
    for (TreeGroup* g : oldGroups) byKey[g->ticket().key] = g;

    std::set<TreeGroup*> consumed;          // 认领登记簿:保证一对一,旧组不被二次复用

    // reuse:输出数组,reuse[i] = 第 i 个新桶配到的旧组;
    // 预置全 nullptr = "没配上,引擎你去 new 一个"
    std::vector<TreeGroup*> reuse(buckets.size(), nullptr);

    // 逐桶认亲:新桶报 key → 旧组索引里找 → 找到且没被认领 → 配对成功
    for (size_t i = 0; i < buckets.size(); ++i) {
        auto it = byKey.find(buckets[i].ticket.key);
        if (it != byKey.end() && !consumed.count(it->second)) {
            reuse[i] = it->second;          // 复用旧指针 → QModelIndex/展开/选中状态存活
            consumed.insert(it->second);    // 登记已认领
        }
    }

    // 清点落选者:没被任何新桶认领的旧组(如 Alum 零件删光了)→ 淘汰名单。
    // 本函数不删它们(纯函数纪律),引擎拿去 dropProjection(级联拆内层)+delete
    for (TreeGroup* g : oldGroups)
        if (!consumed.count(g)) culled.push_back(g);
    return reuse;
}

// ── Stage B-4:组内成员排序 ───────────────────────────
// 职责:每个桶的成员用与全局同一个比较器排序;stable 保证判等元素
//       维持数据侧顺序(不抖动)。
// 输入/输出:members(组 → 成员列表),就地排序;less 空 = 不动
inline void sortMembers(std::map<TreeGroup*, std::vector<GuiBase*>>& members,
                        const Less& less) {
    if (!less) return;                      // 没设比较器 → 保持数据侧顺序,不动
    // kv.first = 组指针,kv.second = 该组的成员列表;逐组就地排序。
    // stable_sort:less 判等的元素保持原有相对顺序(= 数据侧顺序)→ 投影不抖动
    for (auto& kv : members)
        std::stable_sort(kv.second.begin(), kv.second.end(), less);
}

// ── flat 落位 ────────────────────────────────────────
// 职责:把"组"放回它第一个成员原本的位置,直属节点原位不动。
// 输入:vis           = 过滤后的数据侧顺序
//       memberToGroup = 成员 → 组
// 输出:flat(未排序)= 组与直属混排的序列,长度 = vis 中"直属数 + 组数"
inline std::vector<GuiBase*> buildFlat(const std::vector<GuiBase*>& vis,
                                       const std::unordered_map<GuiBase*, TreeGroup*>& memberToGroup) {
    std::vector<GuiBase*> flat;             // 输出:组+直属混排的序列(未排序)
    std::set<TreeGroup*> placed;            // 已落位的组:每个组只进 flat 一次
    // 代表(rep):沿 memberToGroup 上溯到【最外层】组。多层分组时,内层成员的
    // 本层代表是最外层祖先组(Base→Steel组→外层组 ⇒ root 层落"外层组"),
    // 否则内层组会被错误地再落一份到本层。
    auto rep = [&](GuiBase* n) -> GuiBase* {
        GuiBase* cur = n;
        for (;;) {
            auto it = memberToGroup.find(cur);
            if (it == memberToGroup.end()) return cur;
            cur = it->second;
        }
    };
    for (GuiBase* node : vis) {             // 按参与名单顺序逐个扫
        GuiBase* r = rep(node);
        if (r == node) {
            // 直属(数据节点,或未进桶、留在本层的旧组)。
            // ★ 旧组走这里时必须查/登记 placed:多层结构下,外层组可能已经作为
            //   "后代成员的代表"被落过一次(面网格→rep=外层组);迭代到它自身时
            //   若不跳过,同一个组指针进 flat 两遍 → 视图两行同 internalPointer,
            //   点一个选中俩(全量重建后重复节点 bug 的根因)。
            if (auto* gg = dynamic_cast<TreeGroup*>(node)) {
                if (placed.count(gg)) continue;
                placed.insert(gg);
            }
            flat.push_back(node);
            continue;
        }
        auto* g = static_cast<TreeGroup*>(r);
        if (!placed.count(g)) {             // 首次遇到该组成员 → 组落在首成员原位置
            placed.insert(g);
            flat.push_back(g);
        }
    }
    return flat;
}

// ── Stage C:全局排序 ─────────────────────────────────
// 职责:组与直属的混排列表整体 stable_sort。
// 输入/输出:flat 就地排序;less 空 = 保持落位顺序(= 数据侧顺序)
// 注意:less 必须处理 TreeGroup(通常按 ticket.order/label)且严格弱序,
//       否则组的位置不可预期(见 demoLess)。
inline void sortFlat(std::vector<GuiBase*>& flat, const Less& less) {
    // less 为空 → 保持落位顺序(≈数据侧顺序,组在首成员位);
    // 否则整体 stable_sort —— 比较器必须认识 TreeGroup(通常组前置,
    // 按 ticket.order/label 比),判等元素保持落位相对顺序,不抖动
    if (less) std::stable_sort(flat.begin(), flat.end(), less);
}

} // namespace Pipeline
