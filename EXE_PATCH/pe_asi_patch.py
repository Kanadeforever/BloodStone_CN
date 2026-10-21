# -*- coding: utf-8 -*-
"""为 32 位 PE 游戏程序添加三个目录 ASI 的自动扫描入口代码。

设计目标：
  1. 只修改用户明确指定的 EXE 副本，绝不覆写原件。
  2. 不要求 ASI 导出函数，也不安装代理 DLL 或额外启动器。
  3. 利用目标程序已导入的 Windows 系统函数，在游戏入口执行前加载 ASI。
  4. 尽可能兼容不同 PE32 节布局，对无法可靠处理的格式明确报错。

注意：此脚本只负责离线改写与结构验证；不能代替 Windows 实机启动测试。
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys
from dataclasses import dataclass


class PatchError(ValueError):
    """标记由于 PE 格式或补丁条件不满足而产生的可解释错误。"""


def u16(data: bytes | bytearray, offset: int) -> int:
    """读取小端 16 位数值，超出文件边界时由 struct 抛出异常。"""
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes | bytearray, offset: int) -> int:
    """读取小端 32 位数值；PE32 的大多数 RVA 使用这种格式。"""
    return struct.unpack_from("<I", data, offset)[0]


def w16(data: bytearray, offset: int, value: int) -> None:
    """把新的节数量写回 COFF 头部。"""
    struct.pack_into("<H", data, offset, value)


def w32(data: bytearray, offset: int, value: int) -> None:
    """写回入口 RVA、映像大小或其他 PE32 头部字段。"""
    struct.pack_into("<I", data, offset, value)


def align_up(value: int, step: int) -> int:
    """节的文件位置和内存位置必须分别满足对应的对齐要求。"""
    if step <= 0 or step & (step - 1):
        raise PatchError("PE 对齐值不是有效的二次幂")
    return (value + step - 1) & ~(step - 1)


@dataclass(frozen=True)
class Section:
    """记录每个节的虚拟区域、文件区域和权限。"""
    name: bytes
    va: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int


@dataclass(frozen=True)
class PE:
    """封装生成代码所需的头部偏移、节布局及 API 导入信息。"""
    coff: int
    opt: int
    data_dirs: int
    section_table: int
    section_count: int
    file_alignment: int
    section_alignment: int
    header_size: int
    image_base: int
    entry_rva: int
    dll_characteristics: int
    sections: tuple[Section, ...]
    imports: dict[str, dict[str, int]]


def rva_to_offset(data: bytes | bytearray, pe: PE, rva: int, width: int = 1) -> int:
    """按节表将 RVA 转换为文件位置，不把未落盘的虚拟尾部当作有效数据。"""
    if rva < 0 or width < 0:
        raise PatchError("出现负的 RVA 或读取长度")
    if rva + width <= pe.header_size:
        return rva
    for sec in pe.sections:
        valid = sec.raw_size
        if sec.va <= rva and rva + width <= sec.va + valid:
            pos = sec.raw_offset + rva - sec.va
            if pos + width > len(data):
                raise PatchError("RVA 对应的数据超出文件边界")
            return pos
    raise PatchError(f"无法定位 RVA 0x{rva:08X} 对应的文件内容")


def ascii_z(data: bytes | bytearray, offset: int, max_bytes: int = 384) -> str:
    """导入表的 DLL/函数名称是以零结尾的 ASCII 字符串。"""
    end = data.find(b"\0", offset, min(offset + max_bytes, len(data)))
    if end < 0:
        raise PatchError("导入名称缺少终止符")
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise PatchError("导入名称不符合 PE 规定的 ASCII 格式") from exc


def read_pe(data: bytes | bytearray) -> PE:
    """校验 PE32 的头、节、导入项；采集入口引导代码要调用的 API IAT 地址。"""
    if len(data) < 0x200 or data[:2] != b"MZ":
        raise PatchError("输入不是有效的 MZ/EXE 文件")
    pe_header = u32(data, 0x3C)
    if pe_header < 0x40 or pe_header + 24 > len(data) or data[pe_header:pe_header + 4] != b"PE\0\0":
        raise PatchError("找不到有效的 PE 签名")
    coff = pe_header + 4
    section_count = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    opt = coff + 20
    if u16(data, coff) != 0x14C or u16(data, opt) != 0x10B:
        raise PatchError("只支持 Windows x86 PE32，拒绝 x64、ARM 或其他格式")
    if not 1 <= section_count <= 96 or optional_size < 0xE0:
        raise PatchError("PE 节表数量或可选头大小异常")
    if opt + optional_size > len(data):
        raise PatchError("PE 可选头超出文件边界")
    if u32(data, opt + 92) < 16:
        raise PatchError("PE 数据目录数量不足")
    dirs = opt + 96
    section_table = opt + optional_size
    if section_table + section_count * 40 > len(data):
        raise PatchError("PE 节表不完整")

    sections = []
    for i in range(section_count):
        off = section_table + i * 40
        name = bytes(data[off:off + 8])
        virtual_size, va, raw_size, raw_offset = struct.unpack_from("<IIII", data, off + 8)
        flags = u32(data, off + 36)
        if raw_size and (raw_offset < 0x200 or raw_offset + raw_size > len(data)):
            raise PatchError(f"第 {i + 1} 个 PE 节的文件范围异常")
        sections.append(Section(name, va, virtual_size, raw_offset, raw_size, flags))

    pe = PE(coff, opt, dirs, section_table, section_count,
            u32(data, opt + 36), u32(data, opt + 32), u32(data, opt + 60),
            u32(data, opt + 28), u32(data, opt + 16), u16(data, opt + 70),
            tuple(sections), {})
    if pe.entry_rva == 0:
        raise PatchError("该 EXE 没有可用的程序入口")
    if pe.file_alignment < 0x200 or pe.file_alignment > 0x10000:
        raise PatchError("不支持异常的文件对齐值")
    if pe.section_alignment < pe.file_alignment:
        raise PatchError("虚拟节对齐不支持")
    rva_to_offset(data, pe, pe.entry_rva)

    # 数字签名和受保护 EXE 修改后可能无法启动；不盲目破坏原签名。
    cert_pos, cert_len = struct.unpack_from("<II", data, dirs + 4 * 8)
    if cert_pos or cert_len:
        raise PatchError("存在 PE 数字签名目录：不能在保留签名的同时修改 EXE")
    if u32(data, dirs + 14 * 8):
        raise PatchError("检测到托管 CLR 程序，当前工具不支持")

    # 原程序必须已经静态导入基础系统 API，否则需要另一套导入表重构逻辑。
    imp_rva, imp_size = struct.unpack_from("<II", data, dirs + 8)
    if not imp_rva or imp_size < 20:
        raise PatchError("导入表为空或损坏，拒绝修改")
    imp_offset = rva_to_offset(data, pe, imp_rva, 20)
    imports: dict[str, dict[str, int]] = {}
    for i in range(512):
        desc_pos = imp_offset + i * 20
        if desc_pos + 20 > len(data):
            raise PatchError("导入描述符越过文件末尾")
        lookup_rva, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, desc_pos)
        if (lookup_rva, name_rva, first_thunk) == (0, 0, 0):
            break
        dll = ascii_z(data, rva_to_offset(data, pe, name_rva)).lower()
        names = imports.setdefault(dll, {})
        lookup_rva = lookup_rva or first_thunk
        for n in range(4096):
            val = u32(data, rva_to_offset(data, pe, lookup_rva + n * 4, 4))
            if not val:
                break
            if not val & 0x80000000:
                name = ascii_z(data, rva_to_offset(data, pe, val, 2) + 2)
                names[name] = first_thunk + n * 4
        else:
            raise PatchError("一个 DLL 的导入数量异常")
    else:
        raise PatchError("导入描述符数量异常")
    object.__setattr__(pe, "imports", imports)
    return pe


class X86:
    """只生成项目所需的少量 x86 指令，不调用外部汇编工具。"""
    def __init__(self, new_rva: int):
        self.new_rva = new_rva
        self.bytes = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str, str]] = []

    def emit(self, data: bytes) -> None:
        """将已确定编码的机器指令加入节代码。"""
        self.bytes.extend(data)

    def label(self, name: str) -> None:
        """给后续的相对跳转记录一个位置。"""
        if name in self.labels:
            raise PatchError(f"标签重复：{name}")
        self.labels[name] = len(self.bytes)

    def displacement(self, prefix: bytes, name: str, mode: str = "relative") -> None:
        """先留下四字节占位，等字符串和代码位置确定后再回填。"""
        self.emit(prefix)
        pos = len(self.bytes)
        self.emit(b"\0" * 4)
        self.fixups.append((pos, name, mode))

    def data(self, label: str, payload: bytes) -> None:
        """字符串数据放在执行分支之后，正常运行绝不会把字符串当指令执行。"""
        self.label(label)
        self.emit(payload)

    def finish(self, old_entry: int, iat: dict[str, int]) -> bytes:
        """用新增节的 RVA 及原有 IAT 地址回填全部位移。"""
        for pos, name, mode in self.fixups:
            if name == "original_entry":
                address = old_entry
            elif name.startswith("iat:"):
                address = iat[name[4:]]
            else:
                address = self.new_rva + self.labels[name]
            if mode == "relative":
                value = address - (self.new_rva + pos + 4)
                if not -(1 << 31) <= value < 1 << 31:
                    raise PatchError("x86 相对跳转距离溢出")
                struct.pack_into("<i", self.bytes, pos, value)
            elif mode == "rva":
                struct.pack_into("<I", self.bytes, pos, address)
            else:
                raise AssertionError(mode)
        return bytes(self.bytes)


# PE32 入口引导在调用各系统 API 前先保存游戏的入口上下文。
# 所有插件路径均基于游戏 EXE 所在的绝对路径，而不是当前工作目录。
SCAN_DIRS = ("scripts", "CN_Data", "plugins")
PRIORITY_ASI = ("CN_Data\\BloodstoneCN.asi", "CN_Data\\NPCSubtitles.asi")
NEW_SECTION_NAME = b".bcnasi\0"
MAGIC = b"BSCN_ASI_BOOT_V2\0"
OLD_MAGIC = b"BSCN_ASI_BOOT_V1\0"
BUFFER_CHARS = 2048
MAX_MODULE_CHARS = 1720
LOCAL_SIZE = 0x1600
FIND_DATA = 0x1000
FILENAME_FIELD = FIND_DATA + 44
LOCALS = 0x1280
# 这些数字是当前栈缓冲区中的固定偏移，而非游戏内存的固定地址。
API_OFFSETS = {
    "LoadLibraryW": LOCALS + 0x00,
    "GetModuleHandleW": LOCALS + 0x04,
    "GetFileAttributesW": LOCALS + 0x08,
    "FindFirstFileW": LOCALS + 0x0C,
    "FindNextFileW": LOCALS + 0x10,
    "FindClose": LOCALS + 0x14,
    "GetLastError": LOCALS + 0x18,
    "CreateFileW": LOCALS + 0x1C,
    "WriteFile": LOCALS + 0x20,
    "CloseHandle": LOCALS + 0x24,
}
KERNEL_HANDLE = LOCALS + 0x28
LOG_HANDLE = LOCALS + 0x2C
SEARCH_HANDLE = LOCALS + 0x30
LAST_ERROR = LOCALS + 0x34
BYTES_WRITTEN = LOCALS + 0x38
HEX_BUFFER = LOCALS + 0x40
LOG_FILENAME = "BloodStoneASI加载日志.txt"


def make_stub(new_rva: int, old_entry: int, iat: dict[str, int]) -> bytes:
    """生成扫描三个目录的 x86 入口引导代码，不要求游戏额外静态导入任何 DLL。

    Windows 完成原有静态依赖初始化后进入此段代码。它通过游戏既有的
    GetProcAddress 获取系统 API，优先尝试汉化本体与 NPC 插件，再枚举三个
    目录直属的 *.asi。一个插件失败不会终止其他插件或游戏本体。
    """
    a = X86(new_rva)

    def call_api(name: str) -> None:
        # EDI 始终指向栈上的路径缓冲区，各函数指针存储在同一栈区。
        a.emit(b"\xff\x97" + struct.pack("<I", API_OFFSETS[name]))

    def call_local(label: str) -> None:
        a.displacement(b"\xe8", label)

    def copy_path(label: str, char_count: int) -> None:
        # EBP 指向游戏目录末尾的文件名位置；使用 rep movsw 拷贝 UTF-16。
        a.emit(b"\x57")  # push edi
        a.displacement(b"\x8d\xb3", label, "rva")  # lea esi,[ebx+RVA]
        a.emit(b"\x89\xef")  # mov edi,ebp
        a.emit(b"\xb9" + struct.pack("<I", char_count))  # mov ecx,字符数（包括 NUL）
        a.emit(b"\xf3\x66\xa5")  # rep movsw
        a.emit(b"\x5f")  # pop edi

    def write_const(label: str, byte_count: int) -> None:
        a.displacement(b"\x8d\xb3", label, "rva")  # lea esi,[ebx+RVA]
        a.emit(b"\xb9" + struct.pack("<I", byte_count))
        call_local("log_write")

    def wide(text: str) -> bytes:
        return text.encode("utf-16-le")

    a.label("start")
    a.emit(b"\x9c\x60\xfc")  # pushfd / pushad / cld：保存原入口现场并保证 UTF-16 字符串正向复制。
    a.emit(b"\xe8\x00\x00\x00\x00")  # call 下一条，取得运行时地址
    a.label("current_ip")
    a.emit(b"\x5b")  # pop ebx：得到当前位置的绝对地址
    # 当前绝对地址减去它的 RVA，得到当前模块实际 ImageBase。
    a.displacement(b"\x81\xeb", "current_ip", "rva")
    a.emit(b"\x81\xec" + struct.pack("<I", LOCAL_SIZE))  # sub esp,LOCAL_SIZE
    a.emit(b"\x89\xe7")  # mov edi,esp：统一使用路径缓冲区指针。

    # 游戏现有 IAT 已经包含 GetModuleHandleA/GetProcAddress。
    a.displacement(b"\x8d\x83", "kernel32", "rva")
    a.emit(b"\x50")
    a.displacement(b"\xff\x93", "iat:GetModuleHandleA", "rva")
    a.emit(b"\x85\xc0")
    a.displacement(b"\x0f\x84", "restore_original")
    a.emit(b"\x89\x87" + struct.pack("<I", KERNEL_HANDLE))
    for name in API_OFFSETS:
        a.displacement(b"\x8d\x83", "api_" + name, "rva")
        a.emit(b"\x50")
        a.emit(b"\xff\xb7" + struct.pack("<I", KERNEL_HANDLE))  # push [edi+KERNEL_HANDLE]
        a.displacement(b"\xff\x93", "iat:GetProcAddress", "rva")
        a.emit(b"\x85\xc0")
        a.displacement(b"\x0f\x84", "restore_original")
        a.emit(b"\x89\x87" + struct.pack("<I", API_OFFSETS[name]))

    # 先获得游戏 EXE 的完整路径，然后将原文件名位置作为追加目录的起点。
    a.emit(b"\x68" + struct.pack("<I", BUFFER_CHARS))
    a.emit(b"\x57\x6a\x00")
    # WINAPI GetModuleFileNameW(NULL,缓冲区,容量)，参数逆序入栈。
    # 上面顺序为 push 容量 / push 缓冲区 / push NULL。
    a.displacement(b"\xff\x93", "iat:GetModuleFileNameW", "rva")
    a.emit(b"\x85\xc0")
    a.displacement(b"\x0f\x84", "restore_original")
    a.emit(b"\x3d" + struct.pack("<I", MAX_MODULE_CHARS))
    a.displacement(b"\x0f\x83", "restore_original")
    a.emit(b"\x8d\x54\x47\xfe")  # lea edx,[edi+eax*2-2]
    a.label("scan_slash")
    a.emit(b"\x66\x83\x3a\x5c")
    a.displacement(b"\x0f\x84", "found_slash")
    a.emit(b"\x66\x83\x3a\x2f")
    a.displacement(b"\x0f\x84", "found_slash")
    a.emit(b"\x83\xea\x02\x39\xfa")  # sub edx,2; cmp edx,edi
    a.displacement(b"\x0f\x83", "scan_slash")
    a.displacement(b"\xe9", "restore_original")
    a.label("found_slash")
    a.emit(b"\x8d\x6a\x02")  # lea ebp,[edx+2]

    # 生成并打开日志：失败时不阻塞游戏；日志文件在游戏 EXE 同目录。
    copy_path("log_filename", len(LOG_FILENAME) + 1)
    a.emit(b"\x6a\x00")  # hTemplateFile=NULL
    a.emit(b"\x68\x80\x00\x00\x00")  # FILE_ATTRIBUTE_NORMAL
    a.emit(b"\x6a\x02")  # CREATE_ALWAYS
    a.emit(b"\x6a\x00")  # 安全属性=NULL
    a.emit(b"\x6a\x01")  # FILE_SHARE_READ
    a.emit(b"\x68\x00\x00\x00\x40")  # GENERIC_WRITE
    a.emit(b"\x57")  # 文件路径
    call_api("CreateFileW")
    a.emit(b"\x89\x87" + struct.pack("<I", LOG_HANDLE))
    write_const("log_header", len(b"\xff\xfe") + len(wide("BloodStoneCN ASI 保底加载日志\r\n")))

    # 这两个插件先于其他插件尝试加载，保持已验证的初始化顺序。
    for idx, relpath in enumerate(PRIORITY_ASI):
        skip = f"priority_done_{idx}"
        copy_path(f"priority_{idx}", len(relpath) + 1)
        a.emit(b"\x57")
        call_api("GetFileAttributesW")
        a.emit(b"\x83\xf8\xff")  # INVALID_FILE_ATTRIBUTES=-1
        a.displacement(b"\x0f\x84", skip)
        a.emit(b"\xa9\x10\x00\x00\x00")  # 检查 FILE_ATTRIBUTE_DIRECTORY
        a.displacement(b"\x0f\x85", skip)
        call_local("load_one")
        a.label(skip)

    # 使用 FindFirstFileW/FindNextFileW 原生枚举，Windows 文件名匹配不区分大小写。
    for idx, directory in enumerate(SCAN_DIRS):
        prefix = directory + "\\"
        pattern = prefix + "*.asi"
        label_next = f"dir_end_{idx}"
        label_loop = f"dir_loop_{idx}"
        label_copy = f"copy_filename_{idx}"
        label_skip = f"file_skip_{idx}"
        copy_path(f"pattern_{idx}", len(pattern) + 1)
        a.emit(b"\x8d\x87" + struct.pack("<I", FIND_DATA))  # lea eax,[edi+find_data]
        a.emit(b"\x50\x57")  # push find_data; push path
        call_api("FindFirstFileW")
        a.emit(b"\x83\xf8\xff")
        a.displacement(b"\x0f\x84", label_next)
        a.emit(b"\x89\x87" + struct.pack("<I", SEARCH_HANDLE))
        a.label(label_loop)
        a.emit(b"\xf7\x87" + struct.pack("<I", FIND_DATA) + struct.pack("<I", 0x10))
        a.displacement(b"\x0f\x85", label_skip)
        # WIN32_FIND_DATAW.cFileName 位于结构偏移 44，每次把文件名追加到目录前缀。
        a.emit(b"\x8d\xb7" + struct.pack("<I", FILENAME_FIELD))  # lea esi,[edi+filename]
        a.emit(b"\x8d\x95" + struct.pack("<I", 2 * len(prefix)))  # lea edx,[ebp+prefix长度]
        a.label(label_copy)
        a.emit(b"\x66\x8b\x06")  # mov ax,[esi]
        a.emit(b"\x66\x89\x02")  # mov [edx],ax
        a.emit(b"\x83\xc6\x02\x83\xc2\x02")
        a.emit(b"\x66\x85\xc0")  # test ax,ax
        a.displacement(b"\x0f\x85", label_copy)
        call_local("load_one")
        a.label(label_skip)
        a.emit(b"\x8d\x87" + struct.pack("<I", FIND_DATA))
        a.emit(b"\x50")
        a.emit(b"\xff\xb7" + struct.pack("<I", SEARCH_HANDLE))
        call_api("FindNextFileW")
        a.emit(b"\x85\xc0")
        a.displacement(b"\x0f\x85", label_loop)
        a.emit(b"\xff\xb7" + struct.pack("<I", SEARCH_HANDLE))
        call_api("FindClose")
        a.label(label_next)

    # 关闭日志后完整恢复所有寄存器，执行原游戏入口。
    a.emit(b"\xff\xb7" + struct.pack("<I", LOG_HANDLE))
    a.emit(b"\x83\x3c\x24\xff")  # cmp dword [esp],-1
    a.displacement(b"\x0f\x84", "drop_log_handle")
    call_api("CloseHandle")
    a.displacement(b"\xe9", "restore_original")
    a.label("drop_log_handle")
    a.emit(b"\x83\xc4\x04")  # 没打开日志时只撤销 push，不调用 CloseHandle。
    a.label("restore_original")
    a.emit(b"\x81\xc4" + struct.pack("<I", LOCAL_SIZE))
    a.emit(b"\x61\x9d")  # popad / popfd
    a.displacement(b"\xe9", "original_entry")

    # 以下均为内部函数，不由操作系统主动调用；仍在新增节内执行。
    a.label("load_one")
    a.emit(b"\x57")
    call_api("GetModuleHandleW")
    a.emit(b"\x85\xc0")
    a.displacement(b"\x0f\x85", "already_loaded")
    a.emit(b"\x57")
    call_api("LoadLibraryW")
    a.emit(b"\x85\xc0")
    a.displacement(b"\x0f\x84", "load_failed")
    call_local("log_success")
    a.emit(b"\xc3")
    a.label("already_loaded")
    call_local("log_skipped")
    a.emit(b"\xc3")
    a.label("load_failed")
    call_api("GetLastError")
    call_local("log_failure")
    a.emit(b"\xc3")

    # 以 UTF-16 LE 写日志，兼容中文游戏路径与中文插件文件名。
    # 所有日志辅助函数都由 pushad/popad 包裹，防止破坏目录枚举寄存器。
    a.label("log_success")
    a.emit(b"\x60")
    write_const("prefix_success", len(wide("[成功] ")))
    call_local("log_path")
    write_const("line_end", len(wide("\r\n")))
    a.emit(b"\x61\xc3")
    a.label("log_skipped")
    a.emit(b"\x60")
    write_const("prefix_loaded", len(wide("[已加载] ")))
    call_local("log_path")
    write_const("line_end", len(wide("\r\n")))
    a.emit(b"\x61\xc3")
    a.label("log_failure")
    a.emit(b"\x60")
    a.emit(b"\x89\x87" + struct.pack("<I", LAST_ERROR))
    write_const("prefix_failure", len(wide("[失败] ")))
    call_local("log_path")
    write_const("prefix_error", len(wide("  错误码=0x")))
    # 把 GetLastError 的 DWORD 转换成八个 UTF-16 十六进制数字。
    a.emit(b"\x8b\x87" + struct.pack("<I", LAST_ERROR))
    a.emit(b"\x8d\x97" + struct.pack("<I", HEX_BUFFER))
    a.emit(b"\xb9\x08\x00\x00\x00")
    a.label("hex_digit")
    a.emit(b"\xc1\xc0\x04")  # rol eax,4：最高四位依次转到最低位。
    a.emit(b"\x89\xc6\x83\xe6\x0f")
    a.emit(b"\x83\xfe\x09")
    a.displacement(b"\x0f\x87", "hex_alpha")
    a.emit(b"\x83\xc6\x30")
    a.displacement(b"\xe9", "hex_store")
    a.label("hex_alpha")
    a.emit(b"\x83\xc6\x37")
    a.label("hex_store")
    a.emit(b"\x66\x89\x32\x83\xc2\x02\x49")  # mov [edx],si; add edx,2; dec ecx
    a.displacement(b"\x0f\x85", "hex_digit")
    a.emit(b"\x8d\xb7" + struct.pack("<I", HEX_BUFFER))
    a.emit(b"\xb9\x10\x00\x00\x00")
    call_local("log_write")
    write_const("line_end", len(wide("\r\n")))
    a.emit(b"\x61\xc3")

    a.label("log_path")
    a.emit(b"\x89\xfe\x89\xfa")  # mov esi,edi; mov edx,edi
    a.label("measure_path")
    a.emit(b"\x66\x83\x3a\x00")
    a.displacement(b"\x0f\x84", "path_measured")
    a.emit(b"\x83\xc2\x02")
    a.displacement(b"\xe9", "measure_path")
    a.label("path_measured")
    a.emit(b"\x89\xd1\x29\xf1")  # mov ecx,edx; sub ecx,esi
    call_local("log_write")
    a.emit(b"\xc3")

    a.label("log_write")
    a.emit(b"\x83\xbf" + struct.pack("<I", LOG_HANDLE) + b"\xff")
    a.displacement(b"\x0f\x84", "write_end")
    a.emit(b"\x6a\x00")  # lpOverlapped=NULL
    a.emit(b"\x8d\x87" + struct.pack("<I", BYTES_WRITTEN))
    a.emit(b"\x50\x51\x56")  # lpNumberOfBytesWritten, 字节数, 数据地址
    a.emit(b"\xff\xb7" + struct.pack("<I", LOG_HANDLE))
    call_api("WriteFile")
    a.label("write_end")
    a.emit(b"\xc3")

    # 引导专用数据区：绝不在执行期间修改此只读可执行节。
    a.data("kernel32", b"kernel32.dll\0")
    for name in API_OFFSETS:
        a.data("api_" + name, name.encode("ascii") + b"\0")
    a.data("log_filename", wide(LOG_FILENAME + "\0"))
    for idx, relpath in enumerate(PRIORITY_ASI):
        a.data(f"priority_{idx}", wide(relpath + "\0"))
    for idx, directory in enumerate(SCAN_DIRS):
        a.data(f"pattern_{idx}", wide(directory + "\\*.asi\0"))
    a.data("log_header", b"\xff\xfe" + wide("BloodStoneCN ASI 保底加载日志\r\n"))
    a.data("prefix_success", wide("[成功] "))
    a.data("prefix_loaded", wide("[已加载] "))
    a.data("prefix_failure", wide("[失败] "))
    a.data("prefix_error", wide("  错误码=0x"))
    a.data("line_end", wide("\r\n"))
    a.data("magic", MAGIC)
    return a.finish(old_entry, iat)


def patch_bytes(original: bytes) -> tuple[bytes, dict]:
    """复制原有 EXE，追加只读可执行节，并把新节设为入口。"""
    if MAGIC in original or OLD_MAGIC in original:
        raise PatchError("已检测到本工具的引导标记，禁止重复打补丁")
    pe = read_pe(original)
    kernel = pe.imports.get("kernel32.dll", {})
    required_kernel = ("GetModuleHandleA", "GetProcAddress", "GetModuleFileNameW")
    missing = [x for x in required_kernel if x not in kernel]
    if missing:
        raise PatchError("EXE 缺少入口引导所需的原有 API：" + ", ".join(missing))

    # 检查是否有足够的节表余量。扩展节表需要移动文件内容，风险更大，故暂不支持。
    section_header = pe.section_table + pe.section_count * 40
    first_raw = min((s.raw_offset for s in pe.sections if s.raw_size), default=len(original))
    if section_header + 40 > min(pe.header_size, first_raw):
        raise PatchError("节表没有空余 40 字节，拒绝破坏原有节数据")
    if pe.section_count >= 96:
        raise PatchError("节数量超过安全限制")
    highest_va = max(s.va + max(s.virtual_size, s.raw_size) for s in pe.sections)
    new_rva = align_up(highest_va, pe.section_alignment)
    if new_rva >= (1 << 32) - pe.section_alignment:
        raise PatchError("新增节 RVA 超出 PE32 可表示范围")
    iat = {api: kernel[api] for api in required_kernel}
    payload = make_stub(new_rva, pe.entry_rva, iat)
    raw_offset = align_up(len(original), pe.file_alignment)
    raw_size = align_up(len(payload), pe.file_alignment)
    new_size = align_up(new_rva + len(payload), pe.section_alignment)
    if new_size >= (1 << 32):
        raise PatchError("修改后的映像大小溢出")

    # 先复制原文件，再在末尾添加数据，保证所有原有文件偏移不发生迁移。
    result = bytearray(original)
    result.extend(b"\0" * (raw_offset - len(result)))
    result.extend(payload)
    result.extend(b"\0" * (raw_size - len(payload)))
    struct.pack_into("<8sIIIIIIHHI", result, section_header,
                     NEW_SECTION_NAME, len(payload), new_rva, raw_size, raw_offset,
                     0, 0, 0, 0, 0x60000020)  # CODE | EXECUTE | READ，无写入权限。
    w16(result, pe.coff + 2, pe.section_count + 1)
    w32(result, pe.opt + 16, new_rva)  # 修改 EXE 的入口 RVA，原入口字节保持不变。
    w32(result, pe.opt + 56, new_size)
    w32(result, pe.opt + 4, u32(result, pe.opt + 4) + raw_size)  # SizeOfCode。
    w32(result, pe.opt + 64, 0)  # 校验和失效，置零等待后续重算（EXE 非驱动）。

    # 验证输入节的范围未被改动，新节包含所有目标路径，IAT/IMPORT 均未变化。
    patched = read_pe(result)
    if patched.entry_rva != new_rva or patched.section_count != pe.section_count + 1:
        raise PatchError("生成后的 PE 入口或节数量验证失败")
    if patched.sections[:-1] != pe.sections or patched.imports != pe.imports:
        raise PatchError("生成后的原有节或导入表发生意外变化")
    if patched.sections[-1].name != NEW_SECTION_NAME:
        raise PatchError("新增节名验证失败")
    if result[rva_to_offset(result, patched, new_rva, len(payload)):
              rva_to_offset(result, patched, new_rva, len(payload)) + len(payload)] != payload:
        raise PatchError("入口引导代码读回校验失败")
    if not all((directory + "\\*.asi\0").encode("utf-16-le") in payload for directory in SCAN_DIRS):
        raise PatchError("缺少指定扫描目录的通配符路径")
    return bytes(result), {
        "original_entry_rva": pe.entry_rva,
        "new_entry_rva": new_rva,
        "new_section_size": len(payload),
        "imports_kept": len(pe.imports),
        "original_sha256": hashlib.sha256(original).hexdigest(),
        "patched_sha256": hashlib.sha256(result).hexdigest(),
    }


def patch_file(source: pathlib.Path, output: pathlib.Path) -> dict:
    """使用仅创建模式保存输出，避免重复运行时覆盖用户其他补丁或文件。"""
    if source.resolve() == output.resolve():
        raise PatchError("输出不能与原 EXE 相同；必须保留一份完全未修改的原版")
    original = source.read_bytes()
    result, info = patch_bytes(original)
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("xb") as fp:
            fp.write(result)
    except OSError:
        # 不自动删除用户已有的输出文件，尤其不覆盖已有正常游戏版本。
        raise
    # 读回完整输出并验证 SHA-256，确保磁盘上保存的内容与检测后的内容相同。
    if hashlib.sha256(output.read_bytes()).hexdigest() != info["patched_sha256"]:
        raise PatchError("输出文件落盘后校验不一致")
    return info


def main(argv: list[str] | None = None) -> int:
    """解析用户的文件路径并输出可用于核对的校验值。"""
    parser = argparse.ArgumentParser(description="BloodStoneCN ASI 三目录自动扫描入口补丁")
    parser.add_argument("exe", type=pathlib.Path, help="待处理的 32 位游戏 EXE")
    parser.add_argument("--output", type=pathlib.Path, help="新 EXE 路径；默认同目录加 .asi-auto.exe")
    args = parser.parse_args(argv)
    output = args.output or args.exe.with_name(args.exe.stem + ".asi-auto.exe")
    try:
        info = patch_file(args.exe, output)
    except (PatchError, OSError, struct.error, OverflowError) as exc:
        print("[失败] " + str(exc), file=sys.stderr)
        return 1
    print("[完成] 已在 EXE 入口加入三目录 ASI 自动扫描指令")
    print("[优先加载] CN_Data\\BloodstoneCN.asi、CN_Data\\NPCSubtitles.asi")
    print("[扫描目录] scripts、CN_Data、plugins（直属 *.asi）")
    print("[失败处理] 写入 BloodStoneASI加载日志.txt，跳过后继续游戏")
    print(f"[保留] 原有 DLL 导入类别：{info['imports_kept']}")
    print(f"[入口] 原始 0x{info['original_entry_rva']:08X}，新入口 0x{info['new_entry_rva']:08X}")
    print(f"[输出] {output}")
    print(f"[原始 SHA256] {info['original_sha256']}")
    print(f"[输出 SHA256] {info['patched_sha256']}")
    print("[注意] 通过 PE 静态结构校验，不代表已通过 Windows 游戏实机验收")
    return 0


if __name__ == "__main__":
    sys.exit(main())
