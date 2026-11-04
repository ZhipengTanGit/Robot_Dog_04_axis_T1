"""Finite local UDP capture for the robot battery log (no third-party modules)."""
import argparse
import json
import pathlib
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--esp-ip", help="Send PING to open a two-way UDP session")
    parser.add_argument("--esp-port", type=int, default=9001)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 1 <= args.esp_port <= 65535 or args.seconds <= 0:
        parser.error("Invalid port or capture duration")
    records = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        # Do not steal the port from an already-open network debugging tool.
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        sock.bind((args.bind, args.port))
        sock.settimeout(0.5)
        print(f"Listening UDP {args.bind}:{args.port}", flush=True)
        start = time.monotonic()
        next_ping = start
        while time.monotonic() - start < args.seconds:
            now = time.monotonic()
            if args.esp_ip and now >= next_ping:
                sock.sendto(b"PING\r\n", (args.esp_ip, args.esp_port))
                next_ping = now + 5
            try:
                data, peer = sock.recvfrom(65535)
            except socket.timeout:
                continue
            text = data.decode("utf-8", errors="replace")
            records.append({"elapsed_s": round(time.monotonic() - start, 3),
                            "peer_ip": peer[0], "peer_port": peer[1],
                            "bytes": len(data), "text": text})
            print(f"[{peer[0]}:{peer[1]}] {text.rstrip()}", flush=True)
    summary = {"datagrams": len(records),
               "battery_datagrams": sum(r["text"].startswith("BAT t=") for r in records),
               "peers": sorted({r["peer_ip"] for r in records}),
               "socket_closed": True}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text("".join(r["text"] for r in records), encoding="utf-8")
        args.output.with_suffix(".json").write_text(
            json.dumps({"summary": summary, "records": records}, ensure_ascii=False, indent=2),
            encoding="utf-8")
    print("SUMMARY " + json.dumps(summary, ensure_ascii=False), flush=True)
    return 0 if summary["battery_datagrams"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
