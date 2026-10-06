cmake_minimum_required(VERSION 3.16)
project(NavTreeGroup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_PREFIX_PATH "D:/Qt/5.15.2/msvc2019_64")
set(CMAKE_AUTOMOC ON)   # NavTreeModel 带 Q_OBJECT

find_package(Qt5 COMPONENTS Core Widgets Test REQUIRED)

# 控制台 demo:十场景验证(仅 Core —— QAbstractItemModel 在 QtCore 里)
# navtreemodel.h 是 header-only;带 Q_OBJECT 的头必须【显式列进源列表】,
# AUTOMOC 才会扫它(否则只扫 .cpp 及其同名头 → 链接缺 metaObject)
add_executable(navtree_demo
    main.cpp
    treeprojection.cpp
    navtreemodel.h
)
target_link_libraries(navtree_demo Qt5::Core)

# GUI demo:QTreeView 实跑两条更新通道(offscreen 可无显示运行)
add_executable(gui_demo
    gui_demo.cpp
    treeprojection.cpp
    navtreemodel.h
)
target_link_libraries(gui_demo Qt5::Widgets)

# 场景全集:一个 tab 一个 case(透传/类型分组/过滤+材料/多层/精准删除/全量重建/流水线对比)
# --selftest 参数可 offscreen 自动跑 Tab⑤⑥ 并校验,退出码=结果
add_executable(tree_tabs
    tabs_demo.cpp
    treeprojection.cpp
    navtreemodel.h
)
target_link_libraries(tree_tabs Qt5::Widgets)

# 单元测试:18 块,每块打印 职责/输入/输出/校验(仅 Core+Test,无窗口,退出码=失败块数)
# 也是 pipeline.h 各纯函数阶段的活文档
# ★ T18 用 Qt 自带的 QAbstractItemModelTester 校验模型协议 → 需 Qt5::Test
add_executable(unit_tests
    tests.cpp
    treeprojection.cpp
    navtreemodel.h
)
target_link_libraries(unit_tests Qt5::Core Qt5::Test)

# 交互压力测试:三政体混合树 + 五种右键交互(增/删/多选分组/删组/两种全量重建)
add_executable(interactive_demo
    interactive_demo.cpp
    treeprojection.cpp
    navtreemodel.h
)
target_link_libraries(interactive_demo Qt5::Widgets)

# 性能压测:百万节点全场景(建树/冷装/热路径/模型遍历/增量/重建/宽树/prune)
# bench deep = 深链 100k 级单跑(递归栈深风险,进程隔离)
add_executable(bench
    bench.cpp
    treeprojection.cpp
    navtreemodel.h
)
target_link_libraries(bench Qt5::Core)

# 评审方独立探针(2026-10-06 入网评审):实证三个读码疑点,输出 [CONFIRMED]/[OK]
add_executable(probe_review
    probe_review.cpp
    treeprojection.cpp
    navtreemodel.h
)
target_link_libraries(probe_review Qt5::Core)

if(MSVC)
    # /utf-8 :源码含中文注释/字符串,MSVC 默认按 GBK 解码 → 一堆莫名语法错误
    # /W3    :从"几乎关闭"(默认 /W1)提到常规级别。会暴露一批真实问题
    #          (缺 include、未使用变量、有符号/无符号混算…),
    #          也暴露一批 Qt/标准库头的噪声 —— 噪声用 /wd 精确豁免,不整体静音。
#   ★ 故意【不加 /WX】:警告不是错误,先看得见再逐条收拾。
    #   ⚠⚠ 一条硬规矩:**printf 格式串里只能用 ASCII 符号**。
    #      `→ ★ ─ │ ≈ ⇒` 这些 GBK 表示不了的字符会破坏 MSVC 的 printf 格式串
    #      分析(字符串被截断错位)→ C4474「格式串中指定的参数太多」。
    #      ★ 代价是真实的:C4474 被屏蔽 = printf 参数类型/数量检查失效,
    #        以后真的写错参数也不会报。所以格式串里的符号一律换 ASCII,
    #        而不是加 /wd4474 把它静音。
    set(NT_TARGETS navtree_demo gui_demo tree_tabs unit_tests interactive_demo bench probe_review)

    # ── /W3 噪声豁免(逐条,附理由;不要整体静音)──────────────────────
    # 4127: "conditional expression is constant" —— Qt 的 Q_UNUSED / Q_ASSERT
    #       宏与 constexpr 判断必然触发,不是我们的代码问题。
    # 4514: "unreferenced inline function has been removed" —— 头文件里的
    #       未被用到的 inline(库头常见),不占代码空间。
    # 4668: "preprocessor macro is not defined" —— Qt 的 Q_OS_*/Q_CC_* 探测宏,
    #       MSVC 上部分不定义是正常的。
    set(NT_WARN_EXEMPT /wd4127 /wd4514 /wd4668)

    # 4819: "文件包含不能在当前代码页表示的字符,建议存为 Unicode" ── 已实证是误报。
    #   调查过程(2026-10-05,留着免得有人重新怀疑一遍):
    #     ① 已确认 /utf-8 真的在 cl 命令行里(不是配置没生效);
    #     ② 已确认全部源文件是【合法 UTF-8、无 BOM】(用严格 UTF8Encoding 逐字节校验);
    #     ③ 加 UTF-8 BOM 无效(警告数不变)—— 已回退,不留下无意义的文件改动;
    #     ④ 最小复现:纯中文 + /utf-8,**不报** C4819;
    #     ⑤ 真正的触发源 = 源码里的【装饰性 Unicode 符号】(→ ★ ─ │ ┌ └ ≈ ⇒),
    #        这些字符 GBK 无法表示,MSVC 无法确定文件编码 → 保守报警。
    #   ⚠ 为什么不能全换 ASCII:制表线(─────)让 bench 的内存分解表可读性明显变好,
    #     那 28 处 C4819 全部只在【输出文本/注释】里,不影响任何逻辑。
    #   ⚠ 但 printf 【格式串】里的非 ASCII 必须换掉(见下),那是另一回事。
    list(APPEND NT_WARN_EXEMPT /wd4819)

    foreach(t IN LISTS NT_TARGETS)
        target_compile_options(${t} PRIVATE /utf-8 /W3)
        target_compile_options(${t} PRIVATE ${NT_WARN_EXEMPT})
    endforeach()
endif()
