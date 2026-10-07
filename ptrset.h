// ptrset.h —— 指针值专用去重集(开放寻址,零节点分配)
// ─────────────────────────────────────────────────────────────────────────────
// 为什么不用 std::unordered_set:bench A/B 实测(prune-1M,100 万节点)
//     A 纯数据侧全树 DFS              8.8 ms (  8.7 ns/节点)
//     B 同上 + unordered_set 去重   236.9 ms (234.5 ns/节点)  ← 差值 228 ms
//     C 引擎 prune() 实测           344.6 ms
// → unordered_set 的记账占了 prune 的 66%,而"走树"本身只要 8.8 ms。
// 病根有三个,全都是它为"通用 key"付的税:
//   ① 每次 insert 一次 operator new(节点式存储),百万次 malloc;
//   ② 扩容策略是逐级小步长(1→3→7→17…),每次 rehash 全部元素,且 rehash
//      后元素的哈希值不能缓存 → 探测链每次从头比较;
//   ③ 指针低 4 位恒为 0(malloc 16 字节对齐),默认哈希等于恒等 →
//      桶分布依赖 std::hash 恰好乘了个素数,不是可控设计。
//
// 本类只做一件事:把"这个指针见过吗"压到 ~10 ns 以内。手段:
//   · 扁平开放寻址表(线性探测),容量恒为 2 的幂 → 定位 = hash & mask,无链表;
//   · 每个槽一个裸指针(8 B),不存哈希值(省一半内存 + 省一次比较);
//   · 扩容按 2 倍翻(而非逐级小步长)→ 摊还 O(1),且 rehash 成本被摊薄;
//   · clear() 走 memset(几 MB 的 memset 约 1 ms,相对于百万次 malloc 可忽略)。
//
// 生存法则不变:本类只做【指针值比较与哈希】,永不 deref 元素。
//   它会被喂进已经析构/正在析构的野指针(key 可能是僵尸账页),所以
//   "取值不解引用"是硬约束,不是优化。
#ifndef NAVTREEGROUP_PTRSET_H
#define NAVTREEGROUP_PTRSET_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

class PtrSet {
public:
    /**
     * @brief 构造空集;expected>0 时预分配到能装下 expected 个(省掉几次翻倍)
     * @param [in] expected 预计元素数(0 = 不预分配)
     */
    explicit PtrSet(size_t expected = 0) {
        if (expected) {
            reserve(expected);
        }
    }

    /**
     * @brief 插入指针值
     * @param [in] ptr 待插入指针(nullptr 永不进表;只做值记录,不 deref)
     * @return bool true = 此前没见过(调用方据此做"首次访问"动作);
     *         false = 已见过(重复节点/成环,调用方跳过)
     */
    bool insert(const void* ptr) {
        if (!ptr) {
            return false;                     // 空指针永不进表
        }
        if (m_count * 10 >= m_table.size() * 7) {
            grow();                           // 负载因子 0.7 上限
        }
        size_t slotIdx = hash(ptr) & m_mask;
        for (;;) {
            void* existing = m_table[slotIdx];
            if (!existing) {
                m_table[slotIdx] = const_cast<void*>(ptr);
                ++m_count;
                return true;
            }
            if (existing == ptr) {
                return false;                 // 已存在
            }
            slotIdx = (slotIdx + 1) & m_mask; // 线性探测:负载因子≤0.7 保证有空槽
        }
    }

    /**
     * @brief 查询指针值是否在集内(不插入)。prune 收尾用:账本 key 是否被 DFS 走到过
     * @param [in] ptr 待查指针
     * @return bool true = 在集内
     */
    bool contains(const void* ptr) const {
        if (!ptr || m_table.empty()) {
            return false;
        }
        size_t slotIdx = hash(ptr) & m_mask;
        for (;;) {
            void* existing = m_table[slotIdx];
            if (!existing) {
                return false;                 // 探到空槽 = 不在表里
            }
            if (existing == ptr) {
                return true;
            }
            slotIdx = (slotIdx + 1) & m_mask;
        }
    }

    /**
     * @brief 清空但【保留容量】(prune/walkBindings 是反复调用的热路径,
     *        复用容量避免每次重新 malloc 一张百万级表)
     */
    void clear() {
        if (!m_table.empty()) {
            std::memset(m_table.data(), 0, m_table.size() * sizeof(void*));
        }
        m_count = 0;
    }

    /**
     * @brief 预分配容量(hint 只是"大概要装多少",允许随后继续翻倍)
     * @param [in] expected 预计元素数
     */
    void reserve(size_t expected) {
        size_t needed = expected * 10 / 7 + 1;
        size_t capacity = 16;
        while (capacity < needed) {
            capacity <<= 1;
        }
        if (capacity > m_table.size()) {
            m_table.assign(capacity, nullptr);
            m_mask = capacity - 1;
        }
    }

    /**
     * @brief 当前元素数
     * @return size_t 已插入的不重复指针值个数
     */
    size_t size() const { return m_count; }

private:
    // 容量翻倍 + 全表重插。负载因子到 0.7 才翻 → 摊还 O(1)。
    void grow() {
        std::vector<void*> oldTable;
        oldTable.swap(m_table);
        size_t capacity = oldTable.empty() ? 16 : oldTable.size() * 2;
        m_table.assign(capacity, nullptr);
        m_mask = capacity - 1;
        for (void* reinsertPtr : oldTable) {
            if (reinsertPtr) {
                size_t slotIdx = hash(reinsertPtr) & m_mask;
                while (m_table[slotIdx]) {
                    slotIdx = (slotIdx + 1) & m_mask;
                }
                m_table[slotIdx] = reinsertPtr;
            }
        }
    }

    // splitmix64 终混合:把 malloc 的 16 字节对齐(低 4 位恒 0)彻底打散,
    // 同时让高位 ASLR 位参与 —— 指针当 key 绝不能只用恒等哈希。
    static size_t hash(const void* ptr) {
        uint64_t mixed = (uint64_t)(uintptr_t)ptr;
        mixed ^= mixed >> 33; mixed *= 0xff51afd7ed558ccdULL;
        mixed ^= mixed >> 33; mixed *= 0xc4ceb9fe1a85ec53ULL;
        mixed ^= mixed >> 33;
        return (size_t)mixed;
    }

    std::vector<void*> m_table;   // 幂二容量槽数组,nullptr = 空槽
    size_t m_mask = 0;            // m_table.size()-1(定位用:hash & m_mask)
    size_t m_count = 0;           // 当前元素数
};

#endif // NAVTREEGROUP_PTRSET_H
