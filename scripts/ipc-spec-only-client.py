#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# pyright: basic
"""Lsearch IPC 独立客户端（Spec 012 Requirement 2）。

本文件**仅依据规范文档**实现：
  - ipc/proto.h（协议行格式、动词、结果行字段、under64 规则、错误约定）
  - docs/specs/012-ipc-hardening.md（search3 / esc=1 / path 字段转义）
  - docs/specs/002-search-ipc.md（请求行上限、单行/多行响应是否带 END）

它**不 import 也不复制**仓库任何 C++/客户端代码；实现者未阅读
ipc/*.cpp、ipc/client.{h,cpp}、daemon/*.cpp、core/search.cpp 与
scripts/self-test-ipc.sh 的内部实现。

用途：给 self-test-ipc-diff.sh 提供一个"协议文档独立实现"，
与 build/lsearch 对同一查询语料做逐字节差分。

用法示例：
  ipc-spec-only-client.py --sock /run/lsearch.sock --sort name --limit 200 \
      --query report
  ipc-spec-only-client.py --sock S --count --query a
  ipc-spec-only-client.py --sock S --print0 --dirs-only --query a
  ipc-spec-only-client.py --sock S --under /data --query log
"""
import argparse
import base64
import os
import socket
import sys

# —— 规范常量（来自 ipc/proto.h 注释与 Spec 002/012 文本）——
VERBS = [
    "ping", "version", "stats", "search", "search2", "search3", "count2",
    "capabilities", "rebuild", "add-path", "remove-path", "shutdown",
    "get-config", "set-paths", "set-excludes", "set-opts",
]
MULTILINE_END = "END"


class ProtoError(Exception):
    pass


class Client:
    def __init__(self, sock_path):
        self.sock_path = sock_path
        self.sock = None
        self.fh = None
        self.commands = set()

    def connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(self.sock_path)
        self.sock = s
        self.fh = s.makefile("rb")

    def close(self):
        try:
            if self.fh is not None:
                self.fh.close()
        except Exception:
            pass
        try:
            if self.sock is not None:
                self.sock.close()
        except Exception:
            pass

    def _send(self, line: bytes):
        sock = self.sock
        if sock is None:
            raise ProtoError("尚未连接")
        sock.sendall(line + b"\n")

    def _readline(self) -> bytes:
        fh = self.fh
        if fh is None:
            raise ProtoError("尚未连接")
        raw = fh.readline()
        if raw == b"":
            raise ProtoError("连接在响应中途关闭")
        if raw.endswith(b"\n"):
            raw = raw[:-1]
        return raw

    # —— 能力探测 ——
    def capabilities(self):
        self._send(b"capabilities")
        head = self._readline()
        if not head.startswith(b"OK"):
            raise ProtoError("capabilities 失败: %r" % head)
        cmds = set()
        while True:
            line = self._readline()
            if line == MULTILINE_END.encode():
                break
            if line.startswith(b"commands="):
                csv = line[len(b"commands="):].decode("utf-8", "replace")
                cmds = {c for c in csv.split(",") if c}
        self.commands = cmds
        return cmds

    @staticmethod
    def encode_under(path: str) -> bytes:
        # under64：严格标准 base64；空/全量用单字符 "-"。
        if not path:
            return b"-"
        return base64.b64encode(os.fsencode(path))

    # —— 帧读取 ——
    def _read_frame_rows(self, header: bytes):
        """读取 search/search2/search3：返回 (header_fields, rows)。

        结果行按第一个 TAB 之前的字节为 path（v3 时 path 已被转义，故首个
        TAB 必定是字段分隔符）。
        """
        if header.startswith(b"ERR"):
            raise ProtoError(header.decode("utf-8", "replace"))
        if not header.startswith(b"OK"):
            raise ProtoError("非法响应头: %r" % header)
        fields = header[2:].strip().split(b" ")
        rows = []
        while True:
            line = self._readline()
            if line == MULTILINE_END.encode():
                break
            rows.append(line)
        return fields, rows

    def search(self, verb, limit, dirs_only, files_only, sort, under, query):
        """按 proto.h 文法构造并发送请求；返回结果行列表（path 已解码）。"""
        head = "%s %d %d %d %s" % (verb, limit, 1 if dirs_only else 0,
                                   1 if files_only else 0, sort)
        if verb in ("search2", "search3"):
            req = head.encode() + b" " + self.encode_under(under) + b" " + query
        else:
            req = head.encode() + b" " + query
        self._send(req)
        header = self._readline()
        fields, rows = self._read_frame_rows(header)
        escaped = verb == "search3"
        out = []
        for row in rows:
            tab = row.find(b"\t")
            path = row if tab < 0 else row[:tab]
            if escaped:
                path = self.decode_escaped(path)
            out.append(path)
        return fields, out

    @staticmethod
    def decode_escaped(path: bytes) -> bytes:
        """v3 path 字段反转义：\\\\ -> \\, \\t -> TAB, \\n -> LF, \\r -> CR。"""
        out = bytearray()
        i = 0
        n = len(path)
        while i < n:
            c = path[i]
            if c == 0x5C and i + 1 < n:  # backslash
                nxt = path[i + 1]
                if nxt == 0x5C:
                    out.append(0x5C)
                elif nxt == ord("t"):
                    out.append(0x09)
                elif nxt == ord("n"):
                    out.append(0x0A)
                elif nxt == ord("r"):
                    out.append(0x0D)
                else:  # 未知转义：文档未定义，按字面保留
                    out.append(c)
                    out.append(nxt)
                i += 2
                continue
            out.append(c)
            i += 1
        return bytes(out)

    def count(self, dirs_only, files_only, under, query):
        # count2 单行响应，无 END。
        req = (b"count2 %d %d " % (1 if dirs_only else 0, 1 if files_only else 0)
               + self.encode_under(under) + b" " + query)
        self._send(req)
        line = self._readline()
        if line.startswith(b"ERR"):
            raise ProtoError(line.decode("utf-8", "replace"))
        if not line.startswith(b"OK"):
            raise ProtoError("非法响应: %r" % line)
        fields = line[2:].strip().split(b" ")
        total = int(fields[0]) if fields and fields[0] else 0
        capped = bool(int(fields[1])) if len(fields) > 1 else False
        return total, capped

    def ping(self):
        self._send(b"ping")
        line = self._readline()
        if line != b"OK pong":
            raise ProtoError("ping 响应异常: %r" % line)
        return True


def build_argparser():
    p = argparse.ArgumentParser(description="Lsearch IPC 独立客户端（仅依据协议文档）")
    p.add_argument("--sock", required=True, help="Unix socket 路径")
    p.add_argument("--query", default="", help="查询关键词（子串/通配符/re: 前缀）")
    p.add_argument("--limit", type=int, default=200, help="最大返回条数（0=不限）")
    p.add_argument("--sort", default="name", help="排序键 name|path|size|mtime")
    p.add_argument("--dirs-only", action="store_true")
    p.add_argument("--files-only", action="store_true")
    p.add_argument("--under", default="", help="路径子树前缀（内部按 under64 编码）")
    p.add_argument("--count", action="store_true", help="只打印命中总数（count2）")
    p.add_argument("--print0", action="store_true", help="以 NUL 分隔输出")
    p.add_argument("--reverse", action="store_true", help="结果按客户端顺序反转（TUI F7 语义）")
    p.add_argument("--verb", default="auto", choices=["auto", "search", "search2", "search3"])
    return p


def main(argv=None):
    args = build_argparser().parse_args(argv)
    if not args.query:
        sys.stderr.write("缺少关键词\n")
        return 2
    c = Client(args.sock)
    try:
        c.connect()
        query = os.fsencode(args.query)
        if args.count:
            total, capped = c.count(args.dirs_only, args.files_only, args.under, query)
            sys.stdout.buffer.write(("%d\n" % total).encode())
            if capped:
                sys.stderr.write(">= %d (capped)\n" % total)
            return 0 if total > 0 else 1
        verb = args.verb
        if verb == "auto":
            try:
                cmds = c.capabilities()
            except ProtoError:
                cmds = set()
            if "search3" in cmds:
                verb = "search3"
            elif args.under:
                verb = "search2"
            else:
                verb = "search"
        if verb in ("search2", "search3") and not args.under:
            under = ""
        else:
            under = args.under
        _fields, paths = c.search(verb, args.limit, args.dirs_only, args.files_only,
                                  args.sort, under, query)
        if args.reverse:
            paths = list(reversed(paths))
        sep = b"\0" if args.print0 else b"\n"
        out = sys.stdout.buffer
        for p in paths:
            out.write(p + sep)
        return 0 if paths else 1
    except (ProtoError, OSError) as e:
        sys.stderr.write("ipc-spec-only-client 错误: %s\n" % e)
        return 2
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
