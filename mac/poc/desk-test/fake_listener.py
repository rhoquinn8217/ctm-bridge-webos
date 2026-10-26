#!/usr/bin/env python3
"""A pretend DS5-USBIP for the desk test: its agent on 48054 and one bridge.

mode "drop": the bridge connection is closed once the exchange is done, as a
PC that went away. mode "wait": it stays until the Mac side closes it."""
import select
import socket
import struct
import sys
import threading
import time

HDR = struct.Struct('<IHHIIQII')
MAGIC = 0x54424d43
HELLO, HOST_CONFIG, INPUT, OUTPUT, FGET, FREPORT, FSET = 1, 2, 3, 4, 5, 6, 9
OK = 1
CAPS = struct.Struct('<8H64s64s64s64s')
INFO = struct.Struct('<I28s')
HOSTCFG = struct.Struct('<IHHHB16sHBBB26s')
MODE = sys.argv[1] if len(sys.argv) > 1 else 'drop'
AGENT_PORT, BRIDGE_PORT = 48054, 48057
seq = 0
state = {'busid': None}


def log(text):
    print('listener: ' + text, flush=True)


def send(c, t, flags, rid, payload=b''):
    global seq
    seq += 1
    c.sendall(HDR.pack(MAGIC, 1, t, flags, seq, int(time.monotonic() * 1e6), rid, len(payload)) + payload)


def recv_exact(c, n):
    b = b''
    while len(b) < n:
        chunk = c.recv(n - len(b))
        if not chunk:
            return None
        b += chunk
    return b


def recv_msg(c):
    h = recv_exact(c, HDR.size)
    if h is None:
        return None
    magic, ver, t, flags, _, _, rid, plen = HDR.unpack(h)
    if magic != MAGIC or ver != 1:
        log('BAD HEADER')
        return None
    p = recv_exact(c, plen) if plen else b''
    return None if p is None else (t, flags, rid, p)


def text(b):
    return b.split(b'\0')[0].decode()


def bridge(srv):
    c, _ = srv.accept()
    t, flags, rid, p = recv_msg(c)
    if t != HELLO:
        log('FIRST MESSAGE IS TYPE %d, NOT HELLO' % t)
        return
    v = CAPS.unpack_from(p, 0)
    dlen = INFO.unpack_from(p, CAPS.size)[0]
    log('HELLO flags=%d request_id=%d vid=%04x pid=%04x version=%d bus=%d in=%d out=%d feature=%d capsflags=%d '
        'path=%s serial=%s product="%s" maker=%s descriptor=%d bytes; payload %d = %d expected'
        % (flags, rid, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], text(v[8]), text(v[9]), text(v[10]),
           text(v[11]), dlen, len(p), CAPS.size + INFO.size + dlen))
    ready, _, _ = select.select([c], [], [], 0.5)
    log('NOTHING before the settings, as it should be' if not ready else 'DATA CAME BEFORE THE SETTINGS')
    send(c, HOST_CONFIG, OK, 0, HOSTCFG.pack(0, v[4], v[5], v[6], 0, b'', 0xFFFF, 0xFF, 0xFF, 0xFF, b''))
    inputs, asked, replies = 0, False, {}
    while True:
        m = recv_msg(c)
        if m is None:
            log('the Mac side closed the bridge connection after %d input reports' % inputs)
            break
        t, flags, rid, p = m
        if t == INPUT:
            inputs += 1
            if inputs == 1:
                log('first input: id 0x%02x, %d bytes, flags=%d request_id=%d, byte 1 = 0x%02x'
                    % (p[0], len(p), flags, rid, p[1]))
            if inputs == 20 and not asked:
                asked = True
                send(c, OUTPUT, OK, 0, bytes([0x02]) + bytes(range(1, 48)))
                send(c, FGET, OK, 7, bytes([0x05]) + bytes(40))
                send(c, FSET, OK, 8, bytes([0x08]) + bytes(range(1, 11)))
                log('sent output 0x02 (48 bytes), feature get 0x05 (41 bytes, request 7), feature set 0x08 (request 8)')
        elif t == FREPORT:
            replies[rid] = (flags, p)
            log('feature reply to request %d: flags=%d, %d bytes%s'
                % (rid, flags, len(p), (', id 0x%02x' % p[0]) if p else ''))
        else:
            log('UNEXPECTED message type %d' % t)
        if MODE == 'drop' and len(replies) == 2 and inputs >= 50:
            log('closing the bridge connection after %d input reports, as a PC that went away' % inputs)
            c.close()
            return
    c.close()


def agent():
    a = socket.socket()
    a.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    a.bind(('127.0.0.1', AGENT_PORT))
    a.listen(5)
    while True:
        c, _ = a.accept()
        line = b''
        while not line.endswith(b'\n'):
            chunk = c.recv(256)
            if not chunk:
                break
            line += chunk
        cmd = line.decode().strip()
        log('agent got "%s"' % cmd)
        w = cmd.split()
        if w and w[0] == 'STATUS':
            c.sendall(b'OK CTM_AGENT_V1 level=1 min_tv=1\n')
        elif w and w[0] == 'BRIDGE_START':
            good = len(w) == 5 and w[1] == 'ds5_usb' and w[2] == '0' and w[3].startswith('ctm-ds5_usb-1-') \
                and len(w[3]) < 32 and w[4] == 'level=1'
            log('BRIDGE_START is %s' % ('what the TV sends for a cabled DualSense' if good else 'NOT WHAT THE TV SENDS'))
            state['busid'] = w[3] if len(w) > 3 else None
            srv = socket.socket()
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(('127.0.0.1', BRIDGE_PORT))
            srv.listen(1)
            threading.Thread(target=bridge, args=(srv,), daemon=True).start()
            c.sendall(('OK bridge starting port=%d\n' % BRIDGE_PORT).encode())
        elif w and w[0] == 'BRIDGE_STOP':
            log('BRIDGE_STOP names %s' % ('the bridge it started' if w[1:] == [state['busid']] else 'SOMETHING ELSE'))
            c.sendall(b'OK bridge stopped\n')
        else:
            c.sendall(b'ERR unknown command\n')
        c.close()


agent()
