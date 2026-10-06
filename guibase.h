#pragma once
// ═══════════════════════════════════════════════════════════════
//  既有架构(不可改动语义):
//   · GuiBase      —— 导航树展示接口基类,非 QObject!
//   · GeomObject   —— 几何对象,兼 QObject(展示父亲可回退 QObject 树)
//   · DataObject   —— 数据对象,自管数据父子(m_supDataGroup/m_subDataGroups),
//                     visibleChildren 每次访问现场重算
//  本文件相对原始草图只做【语法补全】(返回类型/分号/继承表/const 修正),
//  结构与语义一行未动;为 demo 增加的构造辅助全部以 [DEMO] 标注。
// ═══════════════════════════════════════════════════════════════
#include <QObject>
#include <string>
#include <vector>
#include <algorithm>

class GuiBase {
public:
    explicit GuiBase(GuiBase* parent = nullptr) : m_parentForTree(parent) {}

    // [架构语义补全] GuiBase 对 m_children 拥有管理权:父亲析构连坐子节点。
    // ⚠ 三政体并存的雷区:走 QObject 政体管理孩子的子类(如 GeomObject 派生),
    //   孩子【绝不能】同时进 m_children —— 否则 ~GuiBase 与 ~QObject 双重删除。
    //   本 demo 的 NamedGeom 严格遵守:QObject 孩子只走 QObject 树,
    //   并重写枚举虚函数从 QObject 树取孩子,m_children 保持为空。
    virtual ~GuiBase() {
        for (GuiBase* c : m_children) delete c;
    }

    virtual std::string type() { return "Base"; }

    // 可重写，因为其他节点有自己的数据结构管理方式
    virtual void updateVisibleChilren() { }

    virtual size_t visibleChildrenCount() const {
        return m_children.size();
    }

    virtual GuiBase* visibleChild(size_t index) const {
        return m_children[index];
    }

    virtual int indexInParent() const {
        GuiBase* p = parentForTree();          // [语法补全] 原草图用了未声明的 parent
        if (!p) {
            return -1;
        }
        for (size_t i = 0; i < p->visibleChildrenCount(); ++i) {
            if (p->visibleChild(i) == this) {
                return (int)i;
            }
        }
        return -1;
    }

    virtual GuiBase* parentForTree() const {   // [语法补全] 原草图缺返回类型
        if (m_parentForTree) {
            return m_parentForTree;
        }
        return nullptr;
    }

    // [DEMO] 构造辅助:往 m_children 挂/摘展示孩子。
    // removeTreeChild 只摘不删 —— 摘除后所有权交还调用方(配合 discard/delete 用)
    void addTreeChild(GuiBase* c) { m_children.push_back(c); }
    void removeTreeChild(GuiBase* c) {
        m_children.erase(std::remove(m_children.begin(), m_children.end(), c), m_children.end());
    }

protected:
    // ── 成员标注(仅注释,语义未动)─────────────────────────
    // m_parentForTree:展示树的"显式父亲"提示位,不是所有权。
    //   写:构造时;读:parentForTree() 的第一优先级。三政体下它经常为
    //   null —— DataObject 回退 m_supDataGroup、GeomObject 回退 QObject 树。
    //   投影引擎从不写它("零写入"纪律的底线)。
    GuiBase* m_parentForTree = nullptr;
    // m_children:GuiBase 政体的孩子容器,【带所有权】(~GuiBase 逐个 delete)。
    //   写:addTreeChild/removeTreeChild(只摘不删,所有权交还调用方);
    //   读:默认版 visibleChildrenCount/visibleChild。
    //   ⚠ QObject 政体的孩子绝不能进这里,否则 ~GuiBase 与 ~QObject 双重删除。
    std::vector<GuiBase*> m_children;
    // m_visibleChildren:"当前可见孩子"的缓存暂存区,给重写
    //   updateVisibleChilren() 的子类用(DataObject 每次访问 clear+重填)。
    //   它是【缓存不是账本】:引擎每次都透过虚函数读最新值,
    //   数据侧删节点后它自动变短 —— 这正是"死节点蒸发"机制的物理基础。
    std::vector<GuiBase*> m_visibleChildren;
};

class GeomObject : public GuiBase, public QObject {   // [语法补全] 原草图漏了 GuiBase 继承
public:
    explicit GeomObject(QObject* qparent = nullptr, GuiBase* treeParent = nullptr)
        : GuiBase(treeParent), QObject(qparent) {}

    std::string type() override { return "Geom"; }

    GuiBase* parentForTree() const override {
        if (m_parentForTree) {
            return m_parentForTree;
        }
        // 展示父亲可回退 QObject 树(多继承交叉转换;GeomObject 无 Q_OBJECT,
        // qobject_cast 不可用,dynamic_cast 等价)
        return dynamic_cast<GuiBase*>(QObject::parent());
    }
};

class DataObject : public GuiBase {                   // [语法补全] 原草图漏了继承
public:
    explicit DataObject(DataObject* sup = nullptr, GuiBase* treeParent = nullptr)
        : GuiBase(treeParent), m_supDataGroup(sup) {}

    std::string type() override { return "Data"; }

    // 可重写，因为其他节点有自己的数据结构管理方式
    void updateVisibleChilren() override {
        m_visibleChildren.clear();
        for (GuiBase* child : m_children) {
            m_visibleChildren.emplace_back(child);
        }
        for (DataObject* obj : m_subDataGroups) {
            m_visibleChildren.emplace_back(obj);
        }
        // 注:草图语义如此 —— m_subDataItems 不进 visibleChildren,保持原样
    }

    size_t visibleChildrenCount() const override {
        // [语法补全] const 方法调用非 const 的 updateVisibleChilren,
        // 语义保持草图原样(每次访问现场重算),用 const_cast 打通
        const_cast<DataObject*>(this)->updateVisibleChilren();
        return m_visibleChildren.size();
    }

    GuiBase* visibleChild(size_t index) const override {
        const_cast<DataObject*>(this)->updateVisibleChilren();
        return m_visibleChildren[index];
    }

    GuiBase* parentForTree() const override {
        if (m_parentForTree) {
            return m_parentForTree;
        }
        return m_supDataGroup;
    }

    // ── [DEMO] 数据侧构造/删除辅助(真实工程自有其管理方式,这里只为演示) ──
    void addSubDataGroup(DataObject* o) { m_subDataGroups.push_back(o); }
    void addSubDataItem(DataObject* o) { m_subDataItems.push_back(o); }
    const std::vector<DataObject*>& subDataGroups() const { return m_subDataGroups; }
    DataObject* supDataGroup() const { return m_supDataGroup; }
    // 模拟数据侧删除:从自己的容器摘除并 delete —— 引擎不感知、不干预,
    // 死节点自然从 visibleChildren 枚举中消失(这正是分组方案的安全基础)
    void destroySubDataGroup(DataObject* o) {
        auto it = std::find(m_subDataGroups.begin(), m_subDataGroups.end(), o);
        if (it != m_subDataGroups.end()) {
            m_subDataGroups.erase(it);
            delete o;
        }
    }

private:
    // ── 成员标注(仅注释,语义未动)─────────────────────────
    // 自行管理 DataObject，同时它下面又可能挂载其他的 GuiBase
    // m_supDataGroup:数据侧父亲(真实数据树的所有权方),兼任
    //   parentForTree() 的回退答案(m_parentForTree==null 时读它)。
    //   写:构造时;父亲删孩子(destroySubDataGroup)不反向动它。
    DataObject* m_supDataGroup = nullptr;

    // m_subDataGroups:子数据组容器,【DataObject 自管所有权】(第三政体)。
    //   写:addSubDataGroup/destroySubDataGroup(摘除+delete);
    //   读:updateVisibleChilren() 每次枚举。引擎的 rawChildren 透过虚函数
    //   看到的就是这份名单 —— 删掉节点,下次 childCount 校验活列表就变了,
    //   触发流水线重算,死节点自动蒸发。
    std::vector<DataObject*> m_subDataGroups;
    // m_subDataItems:子数据项容器。按草图语义【不进 visibleChildren】,
    //   demo 中基本闲置,只为忠实还原既有架构。
    std::vector<DataObject*> m_subDataItems;
};
