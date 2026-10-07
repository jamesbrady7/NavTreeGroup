#!/usr/bin/env python
# -*- coding: utf-8 -*-
# ═══════════════════════════════════════════════════════════════════════════
#  run_coverage.py —— 引擎行覆盖率的【可复跑】测量脚本
# ═══════════════════════════════════════════════════════════════════════════
#  为什么用 MinGW g++ 而不是 MSVC:
#    本机 VS2019 只装了 clang-format/clang-tidy,没有 clang-cl,也没有
#    OpenCppCoverage;MSVC 的 cl.exe 本身不给行覆盖率。而 Qt 自带的
#    MinGW g++ 13.1 + gcov 是完整的 —— 且【引擎与 Qt 运行库无关】,
#    所以能把 tests.cpp 编成 "engine-only" 变体做真测量。
#
#  两个关键机关(不写对就会得到错的数字):
#    ① 假 QObject(tools/coverage/QObject,靠 -I 顺序遮蔽 <QObject>):
#       guibase.h 里 GeomObject 继承 QObject,而引擎逻辑不碰 Qt 实现 ——
#       用替身即可编译三政体代码。【交付代码一行不改】。
#    ② COMDAT 合并:pipeline 里的 inline 函数会被链接器去重,只保留【一份】
#       副本 ⇒ 单看某个 .gcno 会得到 "0 行执行" 的假象。必须把两个 TU
#       (tests / treeprojection)的 gcov 结果【逐行取并集】才是真值。
#
#  用法(在仓库根目录或任意位置):
#      python tools/coverage/run_coverage.py
#  可选环境变量:
#      GXX   指定编译器(默认 D:/Qt/Tools/mingw1310_64/bin/g++.exe)
#      GCOV  指定 gcov  (默认同目录)
# ═══════════════════════════════════════════════════════════════════════════
from __future__ import print_function
import io
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # 仓库根

GXX = os.environ.get("GXX", "D:/Qt/Tools/mingw1310_64/bin/g++.exe")
GCOV = os.environ.get("GCOV", os.path.join(os.path.dirname(GXX), "gcov.exe"))
OUT = os.path.join(ROOT, "build", "coverage")           # 产物目录(已在 .gitignore 内)

# 交付给内网的引擎代码(85% 的目标分母)
ENGINE_FILES = [
    "treeprojection.cpp",
    "treeprojection.h",
    "projection_stages.h",
    "ptrset.h",
    "itreesource.h",
    "guibase.h",
]
# 单独列出、不计入目标(需要真实 Qt 运行库,本机无法插桩)
UNMEASURABLE = ["navtreemodel.h"]
# 测试夹具(不是交付代码,仅供参考)
FIXTURE_FILES = ["demonodes.h"]


def run(cmd, cwd=None, check=True):
    p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    out = p.communicate()[0].decode("utf-8", "replace")
    if check and p.returncode != 0:
        print(out)
        raise SystemExit("命令失败(%d):%s" % (p.returncode, " ".join(cmd)))
    return p.returncode, out


def main():
    if not os.path.exists(GXX):
        print("找不到编译器:%s" % GXX)
        return 2
    if os.path.isdir(OUT):
        # ⚠ 上一次的 unit_cov.exe 偶尔还没来得及退出(或杀软在扫),rmtree 会 WinError 32 ——
        #   清理失败不该让整个测量挂掉,忽略并继续(残留文件会被后续编译覆盖)。
        shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    common = ["-std=c++17", "-O0", "--coverage", "-DNT_ENGINE_ONLY",
              # ★ 必须定义 _DEBUG:引擎里 `m_paranoid`(漏报检测)的默认值就是
              #   `#ifdef _DEBUG` —— MinGW 不定义它,于是 paranoid 那一整支
              #   (childCount 的干净页比对 / healIfDrift 的点名分支)永远不进覆盖率。
              #   加上它,本构建等价于"Debug 配置",也正是开发期真正跑的那档。
              "-D_DEBUG",
              "-I" + HERE,          # ★ 必须先于任何真实 Qt 头:让 <QObject> 命中替身
              "-I" + ROOT]

    print("== 1/4 编译 engine-only 变体(tests + treeprojection,无 Qt 运行库)==")
    for src in ["tests", "treeprojection"]:
        run([GXX] + common + ["-c", os.path.join(ROOT, src + ".cpp"),
                              "-o", src + ".o"], cwd=OUT)
    run([GXX, "--coverage", "tests.o", "treeprojection.o",
         "-o", "unit_cov.exe"], cwd=OUT)

    print("== 2/4 跑测试电池(退出码=失败块数)==")
    code, out = run([os.path.join(OUT, "unit_cov.exe")], cwd=OUT, check=False)
    fails = [l for l in out.splitlines() if "[FAIL]" in l or "FAIL(" in l]
    print("  退出码=%d,%s" % (code, "全绿" if code == 0 else "有失败:"))
    for l in fails:
        print("   " + l.strip())

    print("== 3/4 逐 TU 生成 gcov,再合并(COMDAT 去重必须合并)==")
    # 每个 .gcno 单独出一份 .gcov,最后逐行取并集
    merged = {}     # (file, lineno) -> True(已执行) / False(可执行但未执行)
    for stem in ["tests", "treeprojection"]:
        sub = os.path.join(OUT, "gcov_" + stem)
        os.makedirs(sub)
        run([GCOV, "-b", "-c", "-o", OUT, os.path.join(OUT, stem + ".gcno")],
            cwd=sub)
        for name in os.listdir(sub):
            if not name.endswith(".gcov"):
                continue
            path = os.path.join(sub, name)
            for line in io.open(path, encoding="utf-8", errors="replace"):
                m = re.match(r"^\s*([\-\d#=*%]+):\s*(\d+):", line)
                if not m:
                    continue
                cnt, lineno = m.group(1), int(m.group(2))
                if lineno == 0:
                    continue
                if cnt.strip() == "-":
                    continue                      # 不可执行行:两种 TU 都跳过
                executed = cnt.strip() not in ("#####", "=====", "%%%%%")
                key = (name[:-len(".gcov")], lineno)
                merged[key] = merged.get(key, False) or executed

    print("== 4/4 行覆盖率(口径:可执行行中已执行的比例,跨 TU 并集)==")
    per_file = {}
    for (fname, _), ex in merged.items():
        e, t = per_file.get(fname, (0, 0))
        per_file[fname] = (e + (1 if ex else 0), t + 1)

    def report(names, title):
        e = t = 0
        print("  " + title)
        for f in names:
            if f not in per_file:
                print("    %-22s (无插桩数据)" % f)
                continue
            fe, ft = per_file[f]
            e += fe
            t += ft
            print("    %-22s %5d / %5d = %5.1f%%" % (f, fe, ft,
                  100.0 * fe / ft if ft else 0))
        pct = 100.0 * e / t if t else 0.0
        print("    " + "-" * 44)
        print("    %-22s %5d / %5d = %5.1f%%" % ("小计", e, t, pct))
        return pct

    pct_engine = report(ENGINE_FILES, "【引擎】(目标 ≥85%)")
    report(FIXTURE_FILES, "【测试夹具】(不计入目标)")

    print("  " + "【本机不可插桩】")
    for f in UNMEASURABLE:
        print("    %-22s 需要真实 Qt 运行库(QAbstractItemModel)。" % f)
        print("    %-22s 其行为由 MSVC 侧 T15/T16/T18/T19 覆盖,但本脚本测不到。" % "")

    print("")
    ok = pct_engine >= 85.0
    print("结论:引擎行覆盖率 %.1f%% %s 85%% 门槛" %
          (pct_engine, "≥" if ok else "<"))
    print("产物目录:%s" % OUT)
    return 0 if (code == 0 and ok) else 1


if __name__ == "__main__":
    sys.exit(main())
