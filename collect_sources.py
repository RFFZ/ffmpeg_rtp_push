#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
递归抓取工程目录下所有 .cpp / .h / .hpp / .cc / .cxx 等源文件内容，
输出为单个文本文件，方便一次性发给 AI 讨论。

用法:
    python collect_sources.py [工程目录] [-o 输出文件] [-e 排除目录...]

示例:
    python collect_sources.py ./myproject -o sources.txt -e build .git third_party
"""

import os
import argparse
from datetime import datetime

# 需要抓取的源码扩展名（可按需增删）
SOURCE_EXTENSIONS = {
    ".cpp", ".cc", ".cxx", ".c++",
    ".h", ".hpp", ".hxx", ".hh", ".inl",
}

# 默认排除的目录名（避免抓取编译产物或第三方库）
DEFAULT_EXCLUDE_DIRS = {
    ".git", ".svn", ".hg",
    "build", "Build", "out", "bin", "obj",
    "node_modules", "venv", ".venv", "__pycache__",
    ".idea", ".vscode", "cmake-build-debug", "cmake-build-release",
    "Debug", "Release", "x64", "x86",
}

# 单文件大小上限（超过则跳过，避免二进制/超大文件把上下文撑爆）
MAX_FILE_SIZE = 1 * 1024 * 1024  # 1 MB


def collect_files(root_dir, exclude_dirs, exclude_files):
    """递归收集所有目标源文件路径"""
    collected = []
    root_dir = os.path.abspath(root_dir)

    for dirpath, dirnames, filenames in os.walk(root_dir):
        # 原地过滤掉要排除的目录（这样 os.walk 不会进入）
        dirnames[:] = [
            d for d in dirnames
            if d not in exclude_dirs and not d.startswith(".")
        ]

        for filename in filenames:
            ext = os.path.splitext(filename)[1].lower()
            if ext not in SOURCE_EXTENSIONS:
                continue
            if filename in exclude_files:
                continue

            full_path = os.path.join(dirpath, filename)
            if os.path.getsize(full_path) > MAX_FILE_SIZE:
                print(f"[跳过] 文件过大: {full_path}")
                continue

            collected.append(full_path)

    # 按路径排序，输出更稳定
    collected.sort()
    return collected


def safe_read(file_path):
    """尝试多种编码读取文件内容"""
    for encoding in ("utf-8", "utf-8-sig", "gbk", "latin-1"):
        try:
            with open(file_path, "r", encoding=encoding) as f:
                return f.read(), encoding
        except UnicodeDecodeError:
            continue
    # 全都失败就用二进制替换方式读
    with open(file_path, "rb") as f:
        return f.read().decode("utf-8", errors="replace"), "utf-8(replace)"


def write_output(files, root_dir, output_path):
    """把收集到的文件内容写入单个输出文件"""
    root_dir = os.path.abspath(root_dir)
    total_chars = 0

    with open(output_path, "w", encoding="utf-8") as out:
        # 头部信息
        out.write("=" * 80 + "\n")
        out.write(f"# 工程源码汇总\n")
        out.write(f"# 生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        out.write(f"# 根目录: {root_dir}\n")
        out.write(f"# 文件数量: {len(files)}\n")
        out.write("=" * 80 + "\n\n")

        # 目录树概览（方便 AI 了解整体结构）
        out.write("## 文件列表\n\n")
        for f in files:
            rel = os.path.relpath(f, root_dir)
            out.write(f"  - {rel}\n")
        out.write("\n" + "=" * 80 + "\n\n")

        # 逐个文件内容
        for idx, file_path in enumerate(files, 1):
            rel_path = os.path.relpath(file_path, root_dir)
            ext = os.path.splitext(file_path)[1].lstrip(".").lower()
            lang = "cpp" if ext in ("cpp", "cc", "cxx", "c++") else "cpp"

            try:
                content, encoding = safe_read(file_path)
            except Exception as e:
                print(f"[错误] 读取失败 {file_path}: {e}")
                continue

            total_chars += len(content)

            out.write(f"\n{'=' * 80}\n")
            out.write(f"## [{idx}/{len(files)}] {rel_path}\n")
            out.write(f"{'=' * 80}\n\n")
            out.write(f"```{lang}\n")
            out.write(content)
            if not content.endswith("\n"):
                out.write("\n")
            out.write("```\n\n")

        # 尾部统计
        out.write(f"\n{'=' * 80}\n")
        out.write(f"# 汇总结束，共 {len(files)} 个文件，约 {total_chars} 字符\n")
        out.write("=" * 80 + "\n")

    return total_chars


def main():
    global MAX_FILE_SIZE
    parser = argparse.ArgumentParser(
        description="抓取工程目录下所有 .cpp/.h 等源文件内容，整理成单个文件便于发给 AI"
    )
    parser.add_argument(
        "directory", nargs="?", default=".",
        help="工程根目录（默认当前目录）"
    )
    parser.add_argument(
        "-o", "--output", default="sources_dump.txt",
        help="输出文件路径（默认 sources_dump.txt）"
    )
    parser.add_argument(
        "-e", "--exclude", nargs="*", default=[],
        help="额外排除的目录名（如 build third_party）"
    )
    parser.add_argument(
        "--exclude-file", nargs="*", default=[],
        help="排除的文件名（如 CMakeCache.txt）"
    )
    parser.add_argument(
        "--max-size", type=int, default=MAX_FILE_SIZE,
        help=f"单文件最大字节数（默认 {MAX_FILE_SIZE}）"
    )

    args = parser.parse_args()

    if not os.path.isdir(args.directory):
        print(f"[错误] 目录不存在: {args.directory}")
        return 1

    
    MAX_FILE_SIZE = args.max_size

    exclude_dirs = DEFAULT_EXCLUDE_DIRS | set(args.exclude)

    print(f"[信息] 正在扫描: {os.path.abspath(args.directory)}")
    print(f"[信息] 排除目录: {sorted(exclude_dirs)}")

    files = collect_files(args.directory, exclude_dirs, set(args.exclude_file))

    if not files:
        print("[警告] 没有找到任何源文件")
        return 1

    print(f"[信息] 找到 {len(files)} 个源文件，开始写入 {args.output} ...")
    total = write_output(files, args.directory, args.output)

    size_kb = os.path.getsize(args.output) / 1024
    print(f"[完成] 输出文件: {args.output}")
    print(f"[完成] 文件数: {len(files)}，总字符: {total}，输出大小: {size_kb:.1f} KB")

    if size_kb > 500:
        print("[提示] 输出文件较大，可能超出部分 AI 的上下文限制，"
              "可考虑用 -e 排除部分目录，或分模块生成。")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())