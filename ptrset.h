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
    // 初始为空。expected>0 时预分配到能装下 expected 个(省掉几次翻倍)。
    explicit PtrSet(size_t expected = 0) {
        if (expected) reserve(expected);
    }

    // 插入。返回 true = 此前没见过(调用方据此做"首次访问"动作)。
    // 返回 false = 已见过(重复节点/成环,调用方跳过)。
    bool insert(const void* p) {
        if (!p) return false;                     // 空指针永不进表
        if (m_count * 10 >= m_tab.size() * 7) grow();   // 负载因子 0.7 上限
        size_t i = hash(p) & m_mask;
        for (;;) {
            void* cur = m_tab[i];
            if (!cur)      { m_tab[i] = const_cast<void*>(p); ++m_count; return true; }
            if (cur == p)  return false;           // 已存在
            i = (i + 1) & m_mask;                  // 线性探测:负载因子≤0.7 保证有空槽
        }
    }

    // 查询(不插入)。用于 prune 收尾:账本里的 key 是否被 DFS 走到过。
    bool contains(const void* p) const {
        if (!p || m_tab.empty()) return false;
        size_t i = hash(p) & m_mask;
        for (;;) {
            void* cur = m_tab[i];
            if (!cur)     return false;            // 探到空槽 = 不在表里
            if (cur == p) return true;
            i = (i + 1) & m_mask;
        }
    }

    // 清空但【保留容量】。prune/walkBindings 是反复调用的热路径,
    // 复用容量避免每次重新 malloc 一张百万级表。
    void clear() {
        if (!m_tab.empty()) std::memset(m_tab.data(), 0, m_tab.size() * sizeof(void*));
        m_count = 0;
    }

    // 预分配。hint 只是"大概要装多少",允许随后继续翻倍。
    void reserve(size_t expected) {
        size_t need = expected * 10 / 7 + 1;
        size_t cap = 16;
        while (cap < need) cap <<= 1;
        if (cap > m_tab.size()) { m_tab.assign(cap, nullptr); m_mask = cap - 1; }
    }

    size_t size() const { return m_count; }

private:
    // 容量翻倍 + 全表重插。负载因子到 0.7 才翻 → 摊还 O(1)。
    void grow() {
        std::vector<void*> old;
        old.swap(m_tab);
        size_t cap = old.empty() ? 16 : old.size() * 2;
        m_tab.assign(cap, nullptr);
        m_mask = cap - 1;
        for (void* q : old)
            if (q) { size_t i = hash(q) & m_mask; while (m_tab[i]) i = (i + 1) & m_mask; m_tab[i] = q; }
    }

    // splitmix64 终混合:把 malloc 的 16 字节对齐(低 4 位恒 0)彻底打散,
    // 同时让高位 ASLR 位参与 —— 指针当 key 绝不能只用恒等哈希。
    static size_t hash(const void* p) {
        uint64_t x = (uint64_t)(uintptr_t)p;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return (size_t)x;
    }

    std::vector<void*> m_tab;   // 幂二容量,nullptr = 空槽
    size_t m_mask = 0;
    size_t m_count = 0;
};

#endif // NAVTREEGROUP_PTRSET_H