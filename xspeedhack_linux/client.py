import os
import platform
import socket
import struct
import subprocess
import time
from pathlib import Path
from typing import Optional, Sequence

import psutil

try:
    from importlib import resources
except ImportError:  # pragma: no cover
    import importlib_resources as resources  # type: ignore


if platform.system() != "Linux":
    raise OSError("xspeedhack-linux is only supported on Linux")


def _resolve_resource(resource_name: str) -> Path:
    try:
        return Path(resources.files("xspeedhack_linux.bin") / resource_name)
    except AttributeError:
        with resources.path("xspeedhack_linux.bin", resource_name) as resource_path:
            return Path(resource_path)


def _library_path() -> Path:
    path = _resolve_resource("libxspeedhack.so")
    if not path.exists():
        raise FileNotFoundError("libxspeedhack.so not found; build/install the package first")
    return path


def _default_socket_path(pid: int) -> str:
    return f"/tmp/xspeedhack_{pid}.sock"


def validate_speed(value: float) -> None:
    if value < 0:
        raise ValueError("speed must be >= 0")


def get_pid(process_name: str) -> int:
    for proc in psutil.process_iter(["name"]):
        if proc.info.get("name") == process_name:
            return proc.pid
    raise RuntimeError(f"Process {process_name} not open")


def _gdb_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def inject_shared_object(
    pid: int,
    *,
    so_path: Optional[Path] = None,
    gdb_path: str = "gdb",
    socket_path: Optional[str] = None,
) -> None:
    lib_path = Path(so_path or _library_path()).resolve()
    if not lib_path.exists():
        raise FileNotFoundError(f"Shared library not found: {lib_path}")

    gdb_cmd = [gdb_path, "-n", "-q", "-batch", "-ex", f"attach {pid}"]
    if socket_path:
        escaped_path = _gdb_escape(socket_path)
        gdb_cmd += ["-ex", f'call (int)setenv("XSH_SOCKET_PATH","{escaped_path}",1)']
    gdb_cmd += [
        "-ex",
        f'call (void*) dlopen("{_gdb_escape(str(lib_path))}", 1)',
        "-ex",
        "detach",
        "-ex",
        "quit",
    ]

    try:
        subprocess.run(
            gdb_cmd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("gdb not found; install gdb or use launch()") from exc
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or "").strip()
        if detail:
            detail = f": {detail}"
        raise RuntimeError(f"gdb injection failed{detail}") from exc


class SpeedHackClient:
    def __init__(
        self,
        process_name: Optional[str] = None,
        *,
        process_id: Optional[int] = None,
        auto_inject: bool = True,
        gdb_path: str = "gdb",
        socket_path: Optional[str] = None,
        wait_for_socket: float = 1.0,
    ) -> None:
        if process_name is None and process_id is None:
            raise ValueError("Either process_name or process_id must be provided")

        if process_name is not None:
            pid = get_pid(process_name)
        else:
            pid = int(process_id)

        self.pid = pid
        self.socket_path = socket_path or _default_socket_path(pid)
        self.socket: Optional[socket.socket] = None
        self.process: Optional[subprocess.Popen] = None

        if auto_inject:
            inject_shared_object(pid, gdb_path=gdb_path, socket_path=self.socket_path)

        self.socket = self._connect_socket(wait_for_socket)

    @classmethod
    def launch(
        cls,
        cmd: Sequence[str],
        *,
        cwd: Optional[str] = None,
        env: Optional[dict] = None,
        wait_for_socket: float = 1.0,
        socket_path: Optional[str] = None,
    ) -> "SpeedHackClient":
        if isinstance(cmd, (str, bytes)):
            raise TypeError("cmd must be a sequence of arguments, not a string")

        env_vars = os.environ.copy()
        if env:
            env_vars.update(env)

        lib_path = _library_path()
        existing_preload = env_vars.get("LD_PRELOAD", "")
        if existing_preload:
            env_vars["LD_PRELOAD"] = f"{lib_path}:{existing_preload}"
        else:
            env_vars["LD_PRELOAD"] = str(lib_path)

        if socket_path:
            env_vars["XSH_SOCKET_PATH"] = socket_path

        process = subprocess.Popen(list(cmd), cwd=cwd, env=env_vars)
        client = cls(
            process_id=process.pid,
            auto_inject=False,
            socket_path=socket_path,
            wait_for_socket=wait_for_socket,
        )
        client.process = process
        return client

    def _connect_socket(self, timeout: float) -> socket.socket:
        deadline = time.monotonic() + max(timeout, 0.0)
        last_error: Optional[Exception] = None
        while True:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(self.socket_path)
                return sock
            except (FileNotFoundError, ConnectionRefusedError) as exc:
                last_error = exc
                sock.close()
                if time.monotonic() >= deadline:
                    break
                time.sleep(0.05)
            except Exception as exc:  # pragma: no cover - unexpected
                sock.close()
                raise RuntimeError(f"Failed to connect to socket: {exc}") from exc

        detail = f" ({last_error})" if last_error else ""
        raise RuntimeError(f"Timed out waiting for socket {self.socket_path}{detail}")

    def set_speed(self, value: float) -> None:
        validate_speed(value)
        if not self.socket:
            raise RuntimeError("Socket not connected")
        payload = struct.pack("f", float(value))
        self.socket.sendall(payload)

    def close(self) -> None:
        if self.socket is not None:
            try:
                self.socket.close()
            finally:
                self.socket = None

    def __enter__(self) -> "SpeedHackClient":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()


def attach(
    pid: int,
    *,
    gdb_path: str = "gdb",
    socket_path: Optional[str] = None,
    wait_for_socket: float = 1.0,
) -> SpeedHackClient:
    return SpeedHackClient(
        process_id=pid,
        auto_inject=True,
        gdb_path=gdb_path,
        socket_path=socket_path,
        wait_for_socket=wait_for_socket,
    )
