// ═══════════════════════════════════════════════════════════════════════════
//  manual_snippets_check.cpp —— 《docs/部署手册.md》里每一段代码的【可编译性】验证
// ═══════════════════════════════════════════════════════════════════════════
//  为什么要有这个文件:手册的全部价值是"照着抄就能跑"。而手册里的片段没法被
//  单元测试覆盖(它们各自依赖接入方自己的数据层函数),所以它们会随接口演进而
//  **悄悄过期**。这里把手册片段逐段抄进一个 TU,只做语法/API 检查 ——
//  只要引擎接口改了名或改了签名,这里立刻编不过。
//
//  运行(只检查语法,不链接 —— 所以不需要 Qt 运行库,只要有 Qt 头):
//      "D:/Qt/Tools/mingw1310_64/bin/g++.exe" -std=c++17 -fsyntax-only \
//          -I<仓库根> \
//          -I"D:/Qt/5.15.2/msvc2019_64/include" \
//          -I"D:/Qt/5.15.2/msvc2019_64/include/QtCore" \
//          -I"D:/Qt/5.15.2/msvc2019_64/include/QtGui" \
//          -I"D:/Qt/5.15.2/msvc2019_64/include/QtWidgets" \
//          tools/manual_snippets_check.cpp
//      # 无输出 = 全部通过
//
//  ⚠ 本文件【不参与 CMake 构建】(它只是 a syntax-only 检查靶子);
//    里面的 `collectChildren` / `destroyNode` / `attachNewChild` 等是**占位**,
//    对应手册 §0「阅读约定」里说的"接入方自己的数据层函数"。
// ═══════════════════════════════════════════════════════════════════════════
#include <QTreeView>
#include <QString>
#include "guibase.h"
#include "treeprojection.h"
#include "navtreemodel.h"
#include "demonodes.h"   // 真实节点类(PartObject/FolderObject);手册里换成你自己的

// ── 占位:接入方自己的数据层函数(手册 §0 的"阅读约定")──
static bool isPart(GuiBase*);
static void destroyNode(GuiBase*);
static QString displayName(GuiBase*);
static std::vector<GuiBase*> collectChildren(GuiBase*);
static void attachNewChild(GuiBase*, int);

// 逐段对应《部署手册》的章节 —— 顺序与手册一致,方便对照
static void check_all_snippets(GuiBase* root, QTreeView* view, GuiBase* parent,
                               GuiBase* victim, GuiBase* dataParent, PartObject* part)
{
    TreeProjection& P = TreeProjection::instance();

    // ═══ §1 最小接线(五步)═══
    P.registerRoot(root);

    TreeProjection::SpecBinding binding;
    binding.id = "parts-by-material";
    binding.locate = [](GuiBase* n) { return dynamic_cast<PartObject*>(n) != nullptr; };
    binding.spec.filter = [](GuiBase* n) {
        auto* p = dynamic_cast<PartObject*>(n);
        return !(p && p->suppressed);
    };
    binding.spec.grouping = GroupRules::byKey([](GuiBase* n) {
        auto* p = dynamic_cast<PartObject*>(n);
        return p ? p->material() : std::string();
    });
    binding.spec.less = nullptr;
    binding.spec.opt = { /*collapseIfSingleGroup=*/false, /*dynamicAll=*/true };
    P.bindSpec(std::move(binding));

    NavTreeModel model(root, P);
    model.setLabeler([](GuiBase* n) { return displayName(n); });
    view->setModel(&model);

    // ═══ §2 上线建议:首发打开漏报检测 ═══
    P.setParanoidChecks(true);

    // ═══ §2 通知族(四个入口)═══
    P.invalidate(parent);
    P.notifyRemove(dataParent, victim);
    P.invalidateNode(part);
    P.invalidateAll();

    // ═══ §3 一个父节点下的子结点全部删掉、然后重建 ═══
    {
        std::vector<GuiBase*> oldChildren = collectChildren(parent);
        view->setUpdatesEnabled(false);
        model.beginFullReset();
        for (GuiBase* c : oldChildren) { P.notifyRemove(parent, c); destroyNode(c); }
        P.prune();
        for (int i = 0; i < 3; ++i) attachNewChild(parent, i);
        P.rebind();
        model.endFullReset();
        view->doItemsLayout();
        view->setUpdatesEnabled(true);
    }

    // ═══ §4 释放一个父节点下的所有子结点(不重建)═══
    {
        model.beginFullReset();
        for (GuiBase* c : std::vector<GuiBase*>(collectChildren(parent))) {
            P.notifyRemove(parent, c);
            destroyNode(c);
        }
        model.endFullReset();
        view->doItemsLayout();

        // 只想"从视图里拿掉"、不删数据 —— 那是 filter
        ProjectionSpec filtered;
        filtered.filter = [](GuiBase* n) { return isPart(n); };
        model.beginFullReset();
        P.install(parent, filtered);
        model.endFullReset();
        view->doItemsLayout();
    }

    // ═══ §5 精准删除一个节点(事前坐标协议)═══
    {
        GuiBase* dyingGroup = cascadeTarget(P, victim);
        GuiBase* sigParent  = dyingGroup ? P.parentOf(dyingGroup) : P.parentOf(victim);
        int      sigRow     = dyingGroup ? P.rowOf(dyingGroup)    : P.rowOf(victim);
        if (sigRow >= 0) {
            model.beginRowRemoval(sigParent, sigRow);
            P.notifyRemove(dataParent, victim);
            destroyNode(victim);
            model.endRowRemoval();
        } else {
            P.notifyRemove(dataParent, victim);
            destroyNode(victim);
        }
    }

    // ═══ §6 属性变更(无兜底,必须调)═══
    P.invalidateNode(part);

    // ═══ §7 换规则 / 声明式绑定 / 多条规则并存 ═══
    {
        ProjectionSpec newSpec;
        model.beginFullReset();
        P.install(parent, newSpec);
        model.endFullReset();
        view->doItemsLayout();

        P.bindSpec(binding);                 // 同 id 覆盖
        P.unbindSpec("parts-by-material");
        P.rebind();

        GroupRule manual  = [](GuiBase* n) -> std::optional<GroupTicket> {
            auto* p = dynamic_cast<PartObject*>(n);
            if (!p) return std::nullopt;
            GroupTicket t; t.key = "手动组"; t.label = "手动组"; return t;
        };
        GroupRule factory = GroupRules::byKey([](GuiBase* n) {
            auto* p = dynamic_cast<PartObject*>(n);
            return p ? p->material() : std::string();
        });
        ProjectionSpec merged;
        merged.grouping = [manual, factory](GuiBase* n) -> std::optional<GroupTicket> {
            if (auto t = manual(n))  return t;
            if (auto t = factory(n)) return t;
            return std::nullopt;             // 逃生口:不分组,留直属
        };
        P.install(parent, merged);
    }

    // ═══ §8 搜索 / 临时显示被过滤的节点 ═══
    {
        const bool showAll = true;
        auto buildSpec = [&](bool all) {
            ProjectionSpec s;
            s.filter = [all](GuiBase* n) { (void)n; return all; };
            return s;
        };
        model.beginFullReset();
        P.install(root, buildSpec(showAll));
        model.endFullReset();
        view->doItemsLayout();
        volatile int row = P.rowOf(victim);   // <0 = 不在投影树上(仍被过滤)
        (void)row;
    }

    // ═══ §9 多文档 / 多根(谓词自限定)═══
    {
        auto topOf = [](GuiBase* n) {
            GuiBase* p = n;
            while (p) { GuiBase* q = p->parentForTree(); if (!q) break; p = q; }
            return p;
        };
        GuiBase* rootA = root;
        binding.locate = [rootA, topOf](GuiBase* n) { return topOf(n) == rootA && isPart(n); };
        P.unbindSpec("docA-parts");
        P.uninstall(rootA);
        P.unregisterRoot(rootA);
        P.collectGarbage();
    }

    // ═══ §10 收尾 + 观测接口 ═══
    P.unregisterRoot(root);
    P.prune();
    P.collectGarbage();
    const size_t rev  = P.structRev();          (void)rev;
    const size_t ret  = P.retiredCount();       (void)ret;
    const size_t high = P.retiredHighWater();   (void)high;
    const auto   mb   = P.memoryBreakdown();    (void)mb;
    P.collectGarbageSince(0);                   // removeRows 通道用的细粒度回收(见 C3)
}
