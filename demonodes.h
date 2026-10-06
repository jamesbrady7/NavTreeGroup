#pragma once
// ═══════════════════════════════════════════════════════════════
//  demo 节点类 + 演示用比较器(main.cpp 与 gui_demo.cpp 共用)
//  新增子类不算改架构 —— 引擎只依赖 GuiBase 的三个虚接口。
// ═══════════════════════════════════════════════════════════════
#include "guibase.h"
#include "treeprojection.h"   // demoLess 需要认识 TreeGroup(组也要参与排序)
#include <string>
#include <vector>

class NamedData : public DataObject {
public:
    NamedData(std::string name, DataObject* sup = nullptr, GuiBase* treeParent = nullptr)
        : DataObject(sup, treeParent), m_name(std::move(name)) {}
    const std::string& name() const { return m_name; }
private:
    std::string m_name;
};

class PartObject : public NamedData {
public:
    PartObject(std::string name, std::string material, DataObject* sup)
        : NamedData(std::move(name), sup), m_material(std::move(material)) {}
    std::string type() override { return "Part"; }
    const std::string& material() const { return m_material; }
    bool suppressed = false;
private:
    std::string m_material;
};

class SketchObject : public NamedData {
public:
    SketchObject(std::string name, DataObject* sup) : NamedData(std::move(name), sup) {}
    std::string type() override { return "Sketch"; }
};

class ConstraintObject : public NamedData {
public:
    ConstraintObject(std::string name, DataObject* sup) : NamedData(std::move(name), sup) {}
    std::string type() override { return "Constraint"; }
};

// 麻烦构造①:数据层的"遗留分组" —— 真 DataObject、有自己的 subDataGroups,
// 长得像组但不是 TreeGroup,引擎把它当普通节点,其内部永不外泄
class LegacyGroupObject : public NamedData {
public:
    LegacyGroupObject(std::string name, DataObject* sup) : NamedData(std::move(name), sup) {}
    std::string type() override { return "LegacyGroup"; }
};

class FolderObject : public NamedData {
public:
    FolderObject(std::string name, DataObject* sup) : NamedData(std::move(name), sup) {}
    std::string type() override { return "Folder"; }
};

// ═══ QObject 政体的规范用法(最容易被写错的一类) ═══
// 孩子只归 QObject 树管 —— 【绝不】再塞进 m_children,
// 否则 ~GuiBase(连坐 m_children)与 ~QObject(级联删子)双重删除,必炸。
// 枚举重写为从 QObject 树取孩子;m_children 恒为空。
class NamedGeom : public GeomObject {
public:
    NamedGeom(std::string name, QObject* qparent, GuiBase* treeParent)
        : GeomObject(qparent, treeParent), m_name(std::move(name)) {}
    const std::string& name() const { return m_name; }

    size_t visibleChildrenCount() const override { return geomChildren().size(); }
    GuiBase* visibleChild(size_t index) const override { return geomChildren()[index]; }
private:
    std::vector<GuiBase*> geomChildren() const {
        std::vector<GuiBase*> out;
        for (QObject* c : children())
            if (auto* g = dynamic_cast<GuiBase*>(c)) out.push_back(g);
        return out;
    }
    std::string m_name;
};

// ═══ 演示用工具:名字 / 类型秩 / 比较器 ═══

// 统一取显示名(组 → label;数据/几何 → name;其余 → type)
inline std::string demoNodeName(GuiBase* n) {
    if (!n) return "?";
    if (auto* g = dynamic_cast<TreeGroup*>(n)) return g->ticket().label;
    if (auto* d = dynamic_cast<NamedData*>(n)) return d->name();
    if (auto* g = dynamic_cast<NamedGeom*>(n)) return g->name();
    return n->type();
}

// 模拟"写死在旧导航树里的排序":类型秩 → 名字。组永远排最前(秩 -1)。
inline int demoTypeRank(GuiBase* n) {
    if (dynamic_cast<TreeGroup*>(n)) return -1;
    std::string t = n->type();
    if (t == "Geom")        return 0;
    if (t == "Part")        return 1;
    if (t == "Sketch")      return 2;
    if (t == "Constraint")  return 3;
    if (t == "LegacyGroup") return 4;
    if (t == "Folder")      return 5;
    return 6;
}

// 严格弱序 + 确定性 tie-breaker(名字),组间按 (order,label)。
// 比较器必须处理 TreeGroup —— 排序阶段面对的是"组+直属"混合列表。
inline bool demoLess(GuiBase* a, GuiBase* b) {
    auto* ga = dynamic_cast<TreeGroup*>(a);
    auto* gb = dynamic_cast<TreeGroup*>(b);
    if (ga && gb) {
        if (ga->ticket().order != gb->ticket().order) return ga->ticket().order < gb->ticket().order;
        return ga->ticket().label < gb->ticket().label;
    }
    if (ga) return true;    // 组前置于普通节点
    if (gb) return false;
    int ra = demoTypeRank(a), rb = demoTypeRank(b);
    if (ra != rb) return ra < rb;
    return demoNodeName(a) < demoNodeName(b);
}
