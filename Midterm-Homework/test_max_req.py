#!/usr/bin/env python3
"""
tests/multi_echo_one_conn.py
在「同一條 TCP 連線」上連續送多次 ECHO 請求以驗證 max_reqs_per_conn。
協定：
- magic: 'CSB1' -> 0x43534231 (network order)
- type:  REQ_ECHO = 20, RESP_ECHO = 21
- flags: 0
- length: payload bytes (network order)

若伺服器設定 MAX_REQS_PER_CONN=3，則預期第 4 次 request 會遭關閉或讀取失敗。
"""
import argparse
import socket
import struct
import sys
import time

MAGIC = 0x43534231  # 'CSB1'
REQ_ECHO = 20
RESP_ECHO = 21

def pack_hdr(msg_type: int, length: int) -> bytes:
    # !IHHI -> magic(uint32), type(uint16), flags(uint16), length(uint32)
    return struct.pack('!IHHI', MAGIC, msg_type, 0, length)

def recv_exact(sock: socket.socket, n: int) -> bytes:
    """精準讀 n bytes；回傳長度不足代表對端關閉或錯誤。"""
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except socket.timeout:
            raise TimeoutError("recv timeout")
        if not chunk:
            # 對端關閉
            return bytes(buf)
        buf.extend(chunk)
    return bytes(buf)

def send_frame(sock: socket.socket, msg_type: int, payload: bytes) -> None:
    hdr = pack_hdr(msg_type, len(payload))
    sock.sendall(hdr + payload)

def recv_frame(sock: socket.socket, timeout: float = None):
    if timeout is not None:
        sock.settimeout(timeout)
    # 先收 12-byte header
    hdr = recv_exact(sock, 12)
    if len(hdr) == 0:
        # 對端在 header 前就關閉
        return None, None
    if len(hdr) < 12:
        raise RuntimeError(f"short header: got {len(hdr)} bytes")

    magic, typ, flags, length = struct.unpack('!IHHI', hdr)
    if magic != MAGIC:
        raise RuntimeError(f"bad magic: 0x{magic:08x}")
    # 收 payload
    if length > 0:
        data = recv_exact(sock, length)
        if len(data) < length:
            # 對端在 payload 中途關閉
            raise RuntimeError(f"short payload: got {len(data)} < {length}")
    else:
        data = b''

    return typ, data

def main():
    p = argparse.ArgumentParser(description="Send multiple ECHO requests over a single TCP connection.")
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=9090)
    p.add_argument('--count', type=int, default=5, help='要連續送幾次 request（同一連線）')
    p.add_argument('--payload', default='hello', help='ECHO 的字串內容')
    p.add_argument('--delay', type=float, default=0.0, help='每次 request 後等待秒數（便於觀察）')
    p.add_argument('--timeout', type=float, default=5.0, help='recv timeout（秒）')
    args = p.parse_args()

    payload_bytes = args.payload.encode('utf-8')

    print(f"[INFO] connecting to {args.host}:{args.port} ...")
    with socket.create_connection((args.host, args.port)) as s:
        s.settimeout(args.timeout)
        print("[INFO] connected. sending", args.count, "ECHO request(s) on one TCP connection.\n")

        for i in range(1, args.count + 1):
            try:
                print(f"[REQ {i}] -> ECHO '{args.payload}' ({len(payload_bytes)} bytes)")
                send_frame(s, REQ_ECHO, payload_bytes)
                typ, data = recv_frame(s, timeout=args.timeout)
                if typ is None and data is None:
                    print(f"[RESP {i}] peer closed before header (EOF).")
                    break
                if typ != RESP_ECHO:
                    print(f"[RESP {i}] unexpected type={typ}, len={0 if data is None else len(data)}")
                    break
                print(f"[RESP {i}] <- '{data.decode('utf-8', errors='replace')}' ({len(data)} bytes)")
            except (BrokenPipeError, ConnectionResetError) as e:
                print(f"[RESP {i}] connection broken: {e!r}")
                break
            except TimeoutError as e:
                print(f"[RESP {i}] timeout: {e}")
                break
            except Exception as e:
                print(f"[RESP {i}] error: {e!r}")
                break

            if args.delay > 0:
                time.sleep(args.delay)

    print("\n[INFO] done.")

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
