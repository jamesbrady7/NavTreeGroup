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
//     beginRemoveRows → 执行删除(★v4:配合 notifyRemove 通知)→ endRemoveRows。
//     数据变更被 begin/end 正确括号,Qt 协议天然合法(Debug 版 Q_ASSERT 也不会碰)。
//     级联预判:victim 是组的最后一个成员时,整组会蒸发 —— 信号应发在
//     【组的位置】(外层一条),算法见文件尾 cascadeTarget()。范式见 gui_demo 的
//     removeWithSignal()。
//  ② 全量重建 —— beginFullReset() → wipe → prune → rebuild → rebind
//     → endFullReset() → view->doItemsLayout()。
//  兜底:纯移动/排序变化不发精确信号,走通道②或 layoutAboutToBeChanged/
//  layoutChanged(配合持久索引迁移),不要试图用行增删信号硬凑。
#include <QAbstractItemModel>
#include <QString>
#include <QVariant>
#include <functional>
#include "guibase.h"
#include "itreesource.h"

class NavTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    /**
     * @brief 构造模型(不持有 root 与数据源的生死)
     * @param [in] root 不可见根节点(顶层行的父亲;模型对其应用"不可见根规则")
     * @param [in] src 投影引擎的 ITreeSource 窄接口(编译防火墙:模型拿不到写侧刀把)
     * @param [in] parent QObject 父亲(可选)
     */
    NavTreeModel(GuiBase* root, ITreeSource& src, QObject* parent = nullptr)
        : QAbstractItemModel(parent), m_root(root), m_src(src) {}

    /**
     * @brief 注入"节点 → 显示文本"函数(展示口味与引擎解耦;不注入则显示 "?")
     * @param [in] f 标签函数(data(DisplayRole) 时消费)
     */
    void setLabeler(std::function<QString(GuiBase*)> f) { m_labeler = std::move(f); }

    // ── QAbstractItemModel:三要素全部问 ITreeSource ──
    /**
     * @brief 取 parent 下第 row 行的 index(internalPointer = GuiBase*)
     * @param [in] row 行号(Qt 保证先调过 rowCount → 引擎缓存已校验)
     * @param [in] column 列号(本模型单列)
     * @param [in] parent 父亲 index(无效 = 不可见根)
     * @return QModelIndex 有效 index;越界返回无效 index
     */
    QModelIndex index(int row, int col, const QModelIndex& parent) const override {
        GuiBase* childNode = m_src.child(nodeOf(parent), row);   // Qt 保证先调过 rowCount(校验缓存)
        return childNode ? createIndex(row, col, childNode) : QModelIndex();
    }

    /**
     * @brief 取 child 的父亲 index(坐标出自投影账本:parentOf 定父亲,rowOf 定行号)
     * @param [in] child 子 index
     * @return QModelIndex 父亲 index;child 无效或父亲是不可见根 → 返回无效 index
     */
    QModelIndex parent(const QModelIndex& child) const override {
        if (!child.isValid()) {
            return QModelIndex();
        }
        GuiBase* node = nodeOf(child);
        GuiBase* parentNode = m_src.parentOf(node);          // 可能是组;逐层下钻的答案
        if (!parentNode || parentNode == m_root) {
            return QModelIndex();                            // 不可见根规则
        }
        int parentRow = m_src.rowOf(parentNode);
        return parentRow < 0 ? QModelIndex() : createIndex(parentRow, 0, parentNode);
    }

    /**
     * @brief parent 下的行数(= 引擎 childCount,唯一缓存校验点)
     * @param [in] parent 父亲 index(无效 = 不可见根)
     * @return int 投影后的行数(含组节点)
     */
    int rowCount(const QModelIndex& parent) const override {
        return (int)m_src.childCount(nodeOf(parent));
    }

    int columnCount(const QModelIndex&) const override { return 1; }

    /**
     * @brief 取 index 的显示数据(仅 DisplayRole,走注入的 labeler)
     * @param [in] idx 目标 index
     * @param [in] role 数据角色
     * @return QVariant DisplayRole → labeler 文本;其余角色 → 无效 QVariant
     */
    QVariant data(const QModelIndex& idx, int role) const override {
        if (!idx.isValid()) {
            return QVariant();
        }
        if (role == Qt::DisplayRole) {
            return m_labeler ? m_labeler(nodeOf(idx)) : QString("?");
        }
        return QVariant();
    }

    // ── 坐标换算(发 begin/end 信号、程序化定位都用它)──
    /**
     * @brief index → 节点(无效 index = 不可见根)
     * @param [in] idx 源 index
     * @return GuiBase* internalPointer 存的节点;idx 无效时返回 m_root
     */
    GuiBase* nodeOf(const QModelIndex& idx) const {
        return idx.isValid() ? static_cast<GuiBase*>(idx.internalPointer()) : m_root;
    }

    /**
     * @brief 节点 → index(投影坐标;程序化定位/发信号用)
     * @param [in] node 目标节点
     * @return QModelIndex 有效 index;node 为空/是不可见根/不在投影树(rowOf<0)→ 无效 index
     */
    QModelIndex indexFor(GuiBase* node) const {
        if (!node || node == m_root) {
            return QModelIndex();
        }
        int row = m_src.rowOf(node);
        return row < 0 ? QModelIndex() : createIndex(row, 0, node);
    }

    /**
     * @brief 作为"信号里的父亲"用的 index(不可见根 → 无效 index,符合 Qt 协议)
     * @param [in] parentNode 父亲节点(可为空)
     * @return QModelIndex 信号坐标里的 parent 参数
     */
    QModelIndex indexForParent(GuiBase* parentNode) const {
        return (!parentNode || parentNode == m_root) ? QModelIndex() : indexFor(parentNode);
    }

    GuiBase* root() const { return m_root; }
    ITreeSource& src() { return m_src; }

    // ── 全量重建通道的公开门面 ─────────────────────────
    // begin/endResetModel 是 QAbstractItemModel 的 protected 协议,
    // 只能由模型自己暴露 —— 这正是"reset 通道属于模型层职责"的体现。
    // 标准时序:beginFullReset() → wipe → prune → rebuild → rebind → endFullReset()
    //          → view->doItemsLayout()
    /**
     * @brief 全量重建开始(beginResetModel 门面)
     */
    void beginFullReset() { beginResetModel(); }

    /**
     * @brief 全量重建结束:endResetModel 后自动 collectGarbage() ——
     *        此刻 Qt 已令所有 QPersistentModelIndex 失效,才允许释放引擎淘汰的组
     *        ("读侧不释放节点"契约的落地,走 reset 通道的调用方零心智负担)
     */
    void endFullReset() {
        endResetModel();
        m_src.collectGarbage();
    }

    // ── 结构失步自检(惰性投影的契约工具)────────────────
    // 惰性投影的固有事实:一次纯读(rowCount)也可能改变树的形状
    // (成员被数据侧删掉 → 组空 → 组蒸发),而模型收不到任何信号。
    // 调用点范式(见 gui_demo::removeWithSignal):
    //     size_t rev = model.src().structRev();
    //     ... 做一次精准操作 ...
    //     if (model.src().structRev() != rev) { /* 结构被动过 → 本次坐标已失步 */ }
    /**
     * @brief 投影树形状版本号(透传自引擎;精准操作前后各取一次做失步自检)
     * @return size_t 单调递增计数
     */
    size_t structRev() const { return m_src.structRev(); }

    // ── 精准删除通道的公开门面(begin/endRemoveRows 同为 protected)──
    // 事前坐标协议:par/row 必须在 victim 死【前】取自投影账本
    // (parentOf/rowOf),然后 begin → 通知+执行删除 → end。
    // 级联预判:删的是组的最后成员时,整组会蒸发 —— 请把 (par,row)
    // 报在【组的位置】(parentOf(组), rowOf(组)),外层一条信号即可。
    // ★ 多层分组:内层组蒸发可能连锁让外层组也空。多层结构下请用
    //   cascadeTarget()(见文件尾),它沿 parentOf 向上找到【最外层】会蒸发的组;
    //   只判一层会让外层组行消失却无信号 → 模型/视图失步。
    /**
     * @brief 精准删除开始:beginRemoveRows(par 行下的 [row,row])
     * @param [in] par 投影父亲(级联时 = cascadeTarget 给的组)
     * @param [in] row 投影行号(死前取自 rowOf)
     */
    void beginRowRemoval(GuiBase* par, int row) {
        m_retiredAtWindowStart = m_src.retiredCount();   // ★ 记水位(见 endRowRemoval)
        beginRemoveRows(indexForParent(par), row, row);
    }

    /**
     * @brief 精准删除结束:endRemoveRows 后回收【本窗口新退役的】组
     *
     * ⚠ 这里【不能】用整队列的 `collectGarbage()`(2026-10-07 探针 P4 实证):
     *   `endRemoveRows()` 只作废**本次信号覆盖的那个 parent 下 [first,last] 行**的
     *   persistent index,而队列里可能还有更早入队的组 —— 它们的淘汰发生在【纯读】里
     *   (childCount 触发重算时组蒸发),**没有任何信号覆盖过它们的行**。
     *   整队列释放 = 视图仍持有"有效"的 index、而其 internalPointer 已悬空 → UAF。
     *   所以:本窗口新退役的(坐标已被本次信号作废)当场回收,更早的留给下一次
     *   reset 通道(那时 Qt 作废【全部】坐标,整队列释放才安全)。
     */
    void endRowRemoval() {
        endRemoveRows();
        m_src.collectGarbageSince(m_retiredAtWindowStart);
    }

private:
    // ── 成员标注 ──────────────────────────────────────
    // m_root:不可见根节点(模型自己不持有其生死)。
    //   写:构造;读:nodeOf(无效 index) 返回它 —— Qt 问"顶层有几行"
    //     就是问 root 的孩子;parent()/indexFor() 里 parentNode==m_root → 返回
    //     QModelIndex()(不可见根规则,顶层行的 parent 必须是无效 index)。
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
    // m_retiredAtWindowStart:beginRowRemoval 时的退役队列水位。
    //   removeRows 通道只作废它覆盖的行范围,所以 endRowRemoval 只能回收
    //   [本水位, ·) 这一段 —— 更早入队的组其坐标没被任何信号作废(见 endRowRemoval 注释)。
    size_t m_retiredAtWindowStart = 0;
};

// ════════════════════════════════════════════════════════════════
//  级联预判(多层分组的正确算法)—— 三个 demo 共用这一份
// ════════════════════════════════════════════════════════════════
//  为什么不能只判一层(这是外部 review 实证指出的缺陷,已修):
//  多层分组下,删掉一个成员会让【内层组】蒸发;若内层组恰好是【外层组】
//  的唯一成员,外层组也空 → 也蒸发。视图里消失的是两行,只发一条信号
//  → 模型的 rowCount 与视图对不上(失步)。
//
//  算法:沿 parentOf 向上连续检查"childCount==1"的组。
//  一旦某层不满足(该层存活),上面的层一定也存活 —— 因为成员数只减不增到 0,
//  所以【可以提前 break】,不需要走到底。
/**
 * @brief 求"信号应该发在哪一行"= victim 死后会连带蒸发的【最外层】组
 * @param [in] src 引擎窄接口
 * @param [in] victim 将要死的成员节点
 * @return GuiBase* 最外层会蒸发的组(信号发在它的位置,子树随该行消失);
 *         nullptr = 没有组会蒸发(组存活,信号发在成员自己的行)
 */
inline GuiBase* cascadeTarget(ITreeSource& src, GuiBase* victim) {
    GuiBase* outermost = nullptr;
    for (GuiBase* current = src.parentOf(victim); current; current = src.parentOf(current)) {
        if (src.isGroup(current) && src.childCount(current) == 1) {
            outermost = current;               // 这层会蒸发,继续向外找
        } else {
            break;                             // 这层存活 → 更外层也存活,提前收工
        }
    }
    return outermost;
}
