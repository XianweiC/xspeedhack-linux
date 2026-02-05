import argparse
import sys

from .client import SpeedHackClient, attach


def main_launch() -> int:
    parser = argparse.ArgumentParser(description="Launch a command with xspeedhack preloaded")
    parser.add_argument("--speed", type=float, default=1.0, help="Initial speed multiplier")
    parser.add_argument("--wait", type=float, default=3.0, help="Seconds to wait for socket")
    parser.add_argument("cmd", nargs=argparse.REMAINDER, help="Command to run")
    args = parser.parse_args()

    if not args.cmd:
        parser.error("cmd is required")

    client = SpeedHackClient.launch(args.cmd, wait_for_socket=args.wait)
    if args.speed != 1.0:
        client.set_speed(args.speed)
    client.close()
    print(f"pid={client.pid} socket={client.socket_path}")
    return 0


def main_attach() -> int:
    parser = argparse.ArgumentParser(description="Attach xspeedhack to an existing process")
    parser.add_argument("pid", type=int, help="Target process id")
    parser.add_argument("--speed", type=float, default=1.0, help="Initial speed multiplier")
    parser.add_argument("--wait", type=float, default=5.0, help="Seconds to wait for socket")
    args = parser.parse_args()

    client = attach(args.pid, wait_for_socket=args.wait)
    if args.speed != 1.0:
        client.set_speed(args.speed)
    client.close()
    print(f"pid={client.pid} socket={client.socket_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main_launch())
