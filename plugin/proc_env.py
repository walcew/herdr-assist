#!/usr/bin/env python3
"""Lê o ambiente de um processo por pid, cross-platform, só com stdlib.

A ponte usa isto para descobrir CLAUDE_CONFIG_DIR/CODEX_HOME do processo de
cada pane. Nenhum valor além do config-dir é consumido por quem chama (ver
accounts.discover); esta camada só devolve o dict cru.
"""
from __future__ import annotations

import os


def parse_proc_environ(raw: bytes) -> dict:
    """Linux: /proc/<pid>/environ é KEY=VAL separado por NUL."""
    env = {}
    for entry in raw.split(b"\x00"):
        if not entry or b"=" not in entry:
            continue
        k, _, v = entry.partition(b"=")
        if k:
            env[k.decode("utf-8", "replace")] = v.decode("utf-8", "replace")
    return env


def parse_ps_env(text: str) -> dict:
    """macOS: `ps eww -o command=` cospe o argv seguido do env, tudo por espaço.

    Só interessa extrair KEY=VAL; o config-dir não tem espaço, então o split
    simples basta para o que a descoberta precisa.
    """
    env = {}
    for tok in text.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            if k:
                env[k] = v
    return env


def _read_windows(pid: int) -> dict:
    """Windows: caminha o PEB do processo (ctypes) e lê o bloco de ambiente.

    Só processos do mesmo usuário; sem elevação. Qualquer falha vira {}.
    """
    import ctypes as C
    ntdll = C.WinDLL("ntdll")
    k32 = C.WinDLL("kernel32", use_last_error=True)
    PROCESS_QUERY_INFORMATION = 0x0400
    PROCESS_VM_READ = 0x0010

    class PBI(C.Structure):
        _fields_ = [("Reserved1", C.c_void_p), ("PebBaseAddress", C.c_void_p),
                    ("Reserved2", C.c_void_p * 2), ("UniqueProcessId", C.c_void_p),
                    ("Reserved3", C.c_void_p)]

    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        return {}
    try:
        def rd(addr, size):
            buf = (C.c_char * size)()
            got = C.c_size_t(0)
            if not k32.ReadProcessMemory(h, C.c_void_p(addr), buf,
                                         C.c_size_t(size), C.byref(got)):
                raise OSError(C.get_last_error())
            return buf.raw[:got.value]

        def ptr(addr):
            return int.from_bytes(rd(addr, 8), "little")

        pbi = PBI()
        if ntdll.NtQueryInformationProcess(h, 0, C.byref(pbi), C.sizeof(pbi), None) != 0:
            return {}
        peb = C.cast(pbi.PebBaseAddress, C.c_void_p).value
        params = ptr(peb + 0x20)        # PEB.ProcessParameters
        env_ptr = ptr(params + 0x80)    # RTL_USER_PROCESS_PARAMETERS.Environment
        env_len = int.from_bytes(rd(params + 0x3F0, 8), "little")  # EnvironmentSize
        if env_len <= 0 or env_len > (1 << 20):
            env_len = 1 << 16
        text = rd(env_ptr, env_len).decode("utf-16-le", "replace")
        env = {}
        for entry in text.split("\x00"):
            if entry and "=" in entry[1:]:
                k, _, v = entry.partition("=")
                env[k] = v
        return env
    except OSError:
        return {}
    finally:
        k32.CloseHandle(h)


def read_process_env(pid: int) -> dict:
    """Ambiente do processo `pid`, pelo mecanismo do SO. {} em qualquer falha."""
    if not pid or pid <= 0:
        return {}
    try:
        if os.name == "nt":
            return _read_windows(pid)
        if os.path.isdir("/proc"):  # Linux
            with open("/proc/%d/environ" % pid, "rb") as fh:
                return parse_proc_environ(fh.read())
        # macOS e demais BSDs
        import subprocess
        out = subprocess.run(["ps", "eww", "-o", "command=", "-p", str(pid)],
                             capture_output=True, timeout=5)
        return parse_ps_env(out.stdout.decode("utf-8", "replace"))
    except (OSError, ValueError, subprocess.SubprocessError):
        return {}
