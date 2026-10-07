#pragma once
// ═══════════════════════════════════════════════════════════════
//  ITreeSource:展示树的只读窄接口(模型侧唯一依赖)
// ═══════════════════════════════════════════════════════════════
//  目的:QAbstractItemModel 只认识这四个查询,拿不到投影引擎的写侧刀把
//  (apply/install/bindSpec/prune...),用类型系统守住"读侧不许写"的纪律。
//  契约:调用节奏与 Qt 模型一致 —— 先 childCount(触发缓存校验),
//  再 child(直接读缓存);四件套的答案永远出自同一本账(投影),
//  parentOf/rowOf 与 childCount/child 互为逆运算。
#include <cstddef>

class GuiBase;

class ITreeSource {
public:
    virtual ~ITreeSource() = default;

    /**
     * @brief 投影后的孩子数(含组节点);唯一缓存校验点
     * @param [in] node 被查询节点
     * @return size_t 孩子数
     */
    virtual size_t childCount(GuiBase* node) = 0;

    /**
     * @brief 投影序列第 i 个孩子(约定先 childCount 校验再 child 读缓存)
     * @param [in] node 被查询节点
     * @param [in] i 投影序列下标
     * @return GuiBase* 第 i 个;越界返回 nullptr
     */
    virtual GuiBase* child(GuiBase* node, size_t i) = 0;

    /**
     * @brief 投影父亲(可能是组;逐层下钻 deepest-wins)
     * @param [in] node 被定位节点
     * @return GuiBase* 投影父亲
     */
    virtual GuiBase* parentOf(GuiBase* node) = 0;

    /**
     * @brief 在投影父亲序列中的行号
     * @param [in] node 被定位节点
     * @return int 行号;-1 = 不在投影树上(被过滤/已死)
     */
    virtual int rowOf(GuiBase* node) = 0;

    /**
     * @brief 判断节点是否为展示组(TreeGroup)
     * @param [in] node 待判断节点
     * @return bool true = 组节点
     */
    virtual bool isGroup(GuiBase* node) const = 0;

    // ── 生命周期回收(不是"业务写侧刀把")──────────────────────
    // collectGarbage():释放引擎已淘汰、但可能仍被视图以
    //   QPersistentModelIndex.internalPointer 持有的展示节点。
    //   ★ 模型【不】该有任何投影写权限(见文件头),但"坐标失效后回收资源"
    //     是模型层的职责,所以这一条进窄接口 —— 它不改变任何投影内容。
    //   ★ 调用契约:必须在 begin/endResetModel 或 begin/endRemoveRows 之内,
    //     **且要等 end 调用之后**(坐标作废发生在 end,不是 begin)。
    //     NavTreeModel 的 endFullReset / endRowRemoval 已自动调用,
    //     所以走这两条通道的调用方零心智负担。
    /**
     * @brief 释放引擎已淘汰、延迟待回收的展示节点(唯一安全释放点,契约 C3)
     * @note 只有在 reset 通道内才安全:endResetModel 作废【全部】persistent index。
     *       removeRows 通道请改用 collectGarbageSince(见下)。
     */
    virtual void collectGarbage() = 0;

    // ── 回收的两个粒度(2026-10-07 探针 P4 实证后拆分)──────────────
    // ★ 为什么不能一律整队列释放:两条通道的"作废范围"不一样 ——
    //   · endResetModel() 作废【全部】persistent index ⇒ 整队列释放安全;
    //   · endRemoveRows() 只作废【该 parent 下 [first,last] 行】 ⇒ 队列里
    //     更早入队的组(淘汰发生在【纯读】里、没有任何信号覆盖过它的行)
    //     仍然被 Qt 认为"有效",释放它们 = 视图持有悬空 internalPointer。
    //   P4 实测:整体 collectGarbage() 之后,视图侧 held.isValid() 仍为 true。
    /**
     * @brief 当前退役队列水位(beginRowRemoval 时取,传给 collectGarbageSince)
     */
    virtual size_t retiredCount() const = 0;
    /**
     * @brief 只释放 waterMark 之后入队的退役组(removeRows 通道专用)
     * @param [in] waterMark 窗口开始时的退役队列水位
     */
    virtual void collectGarbageSince(size_t waterMark) = 0;

    // ── 结构版本号:用于【检测视图与引擎是否失步】──────────────
    // 惰性投影的固有事实:一次纯读(childCount == rowCount)也可能让树的
    //   形状变化(成员被数据侧删掉 → 组空 → 组蒸发 → 行数变)。
    //   这不是 bug,是"惰性"的定义;但模型不会因此收到任何信号,
    //   所以调用方需要一个手段判断"我这次操作前后结构被动过了没有"。
    //   典型用法:精准操作前后各取一次,不等就说明该走 reset 通道了。
    /**
     * @brief 投影树形状版本号(组增删/行数变化时递增;内容替换不递增)
     * @return size_t 单调递增计数
     */
    virtual size_t structRev() const = 0;
};
