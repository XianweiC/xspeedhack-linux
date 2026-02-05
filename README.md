# xspeedhack-linux

`xspeedhack-linux` is the Linux edition of [`xspeedhack`](https://github.com/loicmagne/xspeedhack).
It provides:
- a preloadable native library (`libxspeedhack.so`) that scales process time/sleep APIs
- a Python client (`import xspeedhack_linux as xsh`) to control speed at runtime

> Safety note: this project hooks runtime APIs. Use only on processes you own and are allowed to modify.

## Highlights
- `LD_PRELOAD` injection for new processes
- optional `gdb` attach injection for already-running processes
- runtime speed control over Unix socket (`float32` protocol)

## Requirements
- Linux x86_64
- Python `>=3.8`
- build tools: C compiler (`gcc`/`clang`), `cmake`
- optional for attach: `gdb`

## Install
```bash
pip install xspeedhack-linux
```

## Quick start

### 1) Launch a process with preload
```python
import xspeedhack_linux as xsh

client = xsh.SpeedHackClient.launch(["/path/to/app", "--flag"])
client.set_speed(2.0)
```

### 2) Attach to existing process
```python
import xspeedhack_linux as xsh

client = xsh.attach(12345)
client.set_speed(0.5)
```

### 3) Legacy-compatible constructor
```python
import xspeedhack_linux as xsh

client = xsh.Client(process_id=12345)
client.set_speed(1.25)
```

### 4) CLI
```bash
xspeedhack-launch --speed 2.0 /path/to/app --flag
xspeedhack-attach --speed 0.5 12345
```

## Runtime behavior
- default scaled clocks: `CLOCK_MONOTONIC*`
- default unscaled clocks: `time()`, `gettimeofday()`, `CLOCK_REALTIME*`
- default socket path: `/tmp/xspeedhack_<pid>.sock`

### Environment variables
- `XSH_SOCKET_PATH`:
  custom Unix socket path
- `XSH_SCALE_REALTIME=1`:
  also scale realtime clocks (`time`, `gettimeofday`, `CLOCK_REALTIME*`)

## Build from source
```bash
python -m pip install -U build
python -m build
```

## Run tests
```bash
python -m pip install -e .[dev]
pytest -q tests
```

## Troubleshooting
- attach fails with ptrace restrictions:
```bash
sudo sysctl -w kernel.yama.ptrace_scope=0
```
- `gdb` not found:
  install `gdb` or use `SpeedHackClient.launch()`
- socket timeout:
  verify preload/attach succeeded and target process is still running

## License
MIT (see `LICENSE`).

## Acknowledgement

See [`xspeedhack`](https://github.com/loicmagne/xspeedhack). Thanks for their excellent project!