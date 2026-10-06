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

    virtual size_t   childCount(GuiBase* node) = 0;   // 投影后的孩子数(含组节点)
    virtual GuiBase* child(GuiBase* node, size_t i) = 0;   // 投影序列第 i 个
    virtual GuiBase* parentOf(GuiBase* node) = 0;     // 投影父亲(可能是组;逐层下钻 deepest-wins)
    virtual int      rowOf(GuiBase* node) = 0;        // 在投影父亲序列中的行号;-1=不在投影树上(被过滤/已死)
    virtual bool     isGroup(GuiBase* node) const = 0;

    // ── 生命周期回收(不是"业务写侧刀把")──────────────────────
    // collectGarbage():释放引擎已淘汰、但可能仍被视图以
    //   QPersistentModelIndex.internalPointer 持有的展示节点。
    //   ★ 模型【不】该有任何投影写权限(见文件头),但"坐标失效后回收资源"
    //     是模型层的职责,所以这一条进窄接口 —— 它不改变任何投影内容。
    //   ★ 调用契约:必须在 begin/endResetModel 或 begin/endRemoveRows 之内。
    //     NavTreeModel 的 endFullReset / endRowRemoval 已自动调用,
    //     所以走这两条通道的调用方零心智负担。
    virtual void collectGarbage() = 0;

    // ── 结构版本号:用于【检测视图与引擎是否失步】──────────────
    // 惰性投影的固有事实:一次纯读(childCount == rowCount)也可能让树的
    //   形状变化(成员被数据侧删掉 → 组空 → 组蒸发 → 行数变)。
    //   这不是 bug,是"惰性"的定义;但模型不会因此收到任何信号,
    //   所以调用方需要一个手段判断"我这次操作前后结构被动过了没有"。
    //   典型用法:精准操作前后各取一次,不等就说明该走 reset 通道了。
    virtual size_t   structRev() const = 0;
};
