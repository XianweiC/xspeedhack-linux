import json
import platform
import sys
import time

import pytest

from xspeedhack_linux import client as xsh_client


def _lib_available() -> bool:
    try:
        path = xsh_client._library_path()
    except FileNotFoundError:
        return False
    return path.exists()


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-only test")
def test_socket_path_default():
    assert xsh_client._default_socket_path(1234) == "/tmp/xspeedhack_1234.sock"


def test_set_speed_bounds():
    with pytest.raises(ValueError):
        xsh_client.validate_speed(-0.1)


@pytest.mark.skipif(platform.system() != "Linux", reason="Linux-only test")
@pytest.mark.skipif(not _lib_available(), reason="Shared library not built")
def test_launch_speedup(tmp_path):
    gate_path = tmp_path / "gate"
    out_path = tmp_path / "out.json"
    script_path = tmp_path / "probe.py"

    script_path.write_text(
        """
import json
import os
import sys
import time

gate = sys.argv[1]
out = sys.argv[2]

while not os.path.exists(gate):
    time.sleep(0.01)

t0_m = time.monotonic()
t0_r = time.time()

time.sleep(0.2)

t1_m = time.monotonic()
t1_r = time.time()

with open(out, "w") as f:
    json.dump({"dm": t1_m - t0_m, "dr": t1_r - t0_r}, f)
""".lstrip()
    )

    client = xsh_client.SpeedHackClient.launch(
        [sys.executable, str(script_path), str(gate_path), str(out_path)],
        wait_for_socket=2.0,
    )
    try:
        client.set_speed(2.0)
        gate_path.write_text("go")

        deadline = time.time() + 5.0
        while not out_path.exists() and time.time() < deadline:
            time.sleep(0.05)

        assert out_path.exists(), "Timed out waiting for output"
        data = json.loads(out_path.read_text())
        ratio = data["dm"] / max(data["dr"], 1e-6)
        assert ratio > 1.4

        if client.process is not None:
            client.process.wait(timeout=5)
    finally:
        client.close()
