#pragma once
// ═══════════════════════════════════════════════════════════════
//  NavTreeModel:QAbstractItemModel 适配层(参考实现,header-only)
// ═══════════════════════════════════════════════════════════════
//  纪律:模型的一切树查询只走 ITreeSource 窄接口 —— 拿不到投影引擎的
//  写侧刀把(apply/install/bindSpec),误用被类型系统拦截。
//  internalPointer 存 GuiBase*(TreeGroup 本身就是 GuiBase,零特殊化)。
//  坐标铁律:QModelIndex 的 row/parent 全部出自投影账本(parentOf/rowOf),
//  绝不能用数据侧原始行号 —— 两本账在分组/过滤/排序后必然不一致。
//
//  两条更新通道的协议(模型侧只需这一薄层,不需要任何 diff 机制):
//  ① 精准删除 —— 【事前坐标】:删除点在 victim 死前问 parentOf/rowOf,
//     beginRemoveRows → 执行删除 → endRemoveRows。数据变更被 begin/end
//     正确括号,Qt 协议天然合法(Debug 版 Q_ASSERT 也不会碰)。
//     级联预判:victim 是组的最后一个成员时,整组会蒸发 —— 信号应发在
//     【组的位置】(外层一条),而不是组内成员行。范式见 gui_demo 的
//     removeWithSignal()。
//  ② 全量重建 —— beginFullReset() → wipe → prune → rebuild → rebind
//     → endFullReset() → view->doItemsLayout()。
//  兜底:纯移动/排序变化不发精确信号,走通道②或 layoutAboutToBeChanged/
//     layoutChanged(配合持久索引迁移),不要试图用行增删信号硬凑。
#include <QAbstractItemModel>
#include <QString>
#include <QVariant>
#include <functional>
#include "guibase.h"
#include "itreesource.h"

class NavTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    NavTreeModel(GuiBase* root, ITreeSource& src, QObject* parent = nullptr)
        : QAbstractItemModel(parent), m_root(root), m_src(src) {}

    void setLabeler(std::function<QString(GuiBase*)> f) { m_labeler = std::move(f); }

    // ── QAbstractItemModel:三要素全部问 ITreeSource ──
    QModelIndex index(int row, int col, const QModelIndex& parent) const override {
        GuiBase* c = m_src.child(nodeOf(parent), row);   // Qt 保证先调过 rowCount(校验缓存)
        return c ? createIndex(row, col, c) : QModelIndex();
    }
    QModelIndex parent(const QModelIndex& child) const override {
        if (!child.isValid()) return QModelIndex();
        GuiBase* n = nodeOf(child);
        GuiBase* p = m_src.parentOf(n);                  // 可能是组;逐层下钻的答案
        if (!p || p == m_root) return QModelIndex();     // 不可见根规则
        int r = m_src.rowOf(p);
        return r < 0 ? QModelIndex() : createIndex(r, 0, p);
    }
    int rowCount(const QModelIndex& parent) const override {
        return (int)m_src.childCount(nodeOf(parent));    // 唯一缓存校验点
    }
    int columnCount(const QModelIndex&) const override { return 1; }
    QVariant data(const QModelIndex& idx, int role) const override {
        if (!idx.isValid()) return QVariant();
        if (role == Qt::DisplayRole)
            return m_labeler ? m_labeler(nodeOf(idx)) : QString("?");
        return QVariant();
    }

    // ── 坐标换算(发 begin/end 信号、程序化定位都用它) ──
    GuiBase* nodeOf(const QModelIndex& idx) const {
        return idx.isValid() ? static_cast<GuiBase*>(idx.internalPointer()) : m_root;
    }
    QModelIndex indexFor(GuiBase* n) const {             // 节点 → index(投影坐标)
        if (!n || n == m_root) return QModelIndex();
        int r = m_src.rowOf(n);
        return r < 0 ? QModelIndex() : createIndex(r, 0, n);
    }
    QModelIndex indexForParent(GuiBase* p) const {       // 作为"信号里的父亲"用
        return (!p || p == m_root) ? QModelIndex() : indexFor(p);
    }
    GuiBase* root() const { return m_root; }
    ITreeSource& src() { return m_src; }

    // ── 全量重建通道的公开门面 ──
    // begin/endResetModel 是 QAbstractItemModel 的 protected 协议,
    // 只能由模型自己暴露 —— 这正是"reset 通道属于模型层职责"的体现。
    // 标准时序:beginFullReset() → wipe → prune → rebuild → rebind → endFullReset()
    //          → view->doItemsLayout()
    //
    // ★ endFullReset 末尾自动 collectGarbage():endResetModel 之后 Qt 已让所有
    //   QPersistentModelIndex 失效,此刻才允许释放引擎淘汰掉的组。
    //   这是"读侧不释放节点"契约的落地 —— 调用方走 reset 通道即自动履约。
    void beginFullReset() { beginResetModel(); }
    void endFullReset()   { endResetModel(); m_src.collectGarbage(); }

    // ── 结构失步自检(惰性投影的契约工具)────────────────────────
    // 惰性投影的固有事实:一次纯读(rowCount)也可能改变树的形状
    // (成员被数据侧删掉 → 组空 → 组蒸发),而模型收不到任何信号。
    // 调用点范式(见 gui_demo::removeWithSignal):
    //     size_t rev = model.src().structRev();
    //     ... 做一次精准操作 ...
    //     if (model.src().structRev() != rev) { /* 结构被动过 → 本次坐标已失步 */ }
    size_t structRev() const { return m_src.structRev(); }

    // ── 精准删除通道的公开门面(begin/endRemoveRows 同为 protected)──
    // 事前坐标协议:par/row 必须在 victim 死【前】取自投影账本
    // (parentOf/rowOf),然后 begin → 执行删除 → end。
    // 级联预判:删的是组的最后成员时,整组会蒸发 —— 请把 (par,row)
    // 报在【组】的位置(parentOf(组), rowOf(组)),外层一条信号即可。
    // ★ 多层分组:内层组蒸发可能连锁让外层组也空。多层结构下请用
    //   cascadeTarget()(见下),它沿 parentOf 向上找到【最外层】会蒸发的组;
    //   只判一层会让外层组行消失却无信号 → 模型/视图失步。
    void beginRowRemoval(GuiBase* par, int row) {
        beginRemoveRows(indexForParent(par), row, row);
    }
    // endRemoveRows 之后,被删行(含随之蒸发的组行)的 persistent index 已失效,
    //   此时回收是安全的(与 endFullReset 同理)。
    void endRowRemoval() { endRemoveRows(); m_src.collectGarbage(); }

private:
    // ── 成员标注 ──────────────────────────────────────────
    // m_root:不可见根节点(模型自己不持有其生死)。
    //   写:构造;读:nodeOf(无效 index) 返回它 —— Qt 问"顶层有几行"
    //   就是问 root 的孩子;parent()/indexFor() 里 p==m_root → 返回
    //   QModelIndex()(不可见根规则,顶层行的 parent 必须是无效 index)。
    GuiBase* m_root;
    // m_src:ITreeSource 窄接口引用 —— 模型对引擎的【全部认知】。
    //   每个 override 都用它:index→child、parent→parentOf+rowOf、
    //   rowCount→childCount(唯一缓存校验点)。类型是接口而非
    //   TreeProjection → 模型拿不到 apply/bindSpec 等写侧刀把,
    //   误用被类型系统拦截(编译防火墙)。
    ITreeSource& m_src;
    // m_labeler:节点 → 显示文本的注入函数(展示口味与引擎解耦)。
    //   写:setLabeler;读:data(DisplayRole)。不注入则显示 "?"。
    std::function<QString(GuiBase*)> m_labeler;
};

// ════════════════════════════════════════════════════════════════
//  级联预判(多层分组的正确算法)—— 三个 demo 共用这一份
// ════════════════════════════════════════════════════════════════
//  返回:信号应该发在【哪一行】,即"最外层那个会随之蒸发的组";
//        nullptr = 没有组会蒸发(组存活,信号发在成员自己的行)。
//  输入:src = 引擎窄接口、victim = 将要死的成员。
//
//  为什么不能只判一层(这是外部 review 实证指出的缺陷,已修):
//  多层分组下,删掉一个成员会让【内层组】蒸发;若内层组恰好是【外层组】
//  的唯一成员,外层组也空 → 也蒸发。视图里消失的是两行,只发一条信号
//  → 模型的 rowCount 与视图对不上(失步)。
//
//  算法:沿 parentOf 向上连续检查"childCount==1"的组。
//  一旦某层不满足(该层存活),上面的层一定也存活 —— 因为成员数只减不增到 0,
//  所以【可以提前 break】,不需要走到底。
inline GuiBase* cascadeTarget(ITreeSource& src, GuiBase* victim) {
    GuiBase* outermost = nullptr;
    for (GuiBase* cur = src.parentOf(victim); cur; cur = src.parentOf(cur)) {
        if (src.isGroup(cur) && src.childCount(cur) == 1) outermost = cur;   // 会蒸发
        else break;                      // 这层存活 → 更外层也存活,提前收工
    }
    return outermost;
}
