#!/usr/bin/env python3
"""ESL load mock: one FreeSWITCH node emitting N calls (result [+message_end] + hangup) as fast as possible.
   load_esl.py <port> <calls>   prints commands received count when the controller disconnects."""
import socket, sys, threading, time, urllib.parse
port, N = int(sys.argv[1]), int(sys.argv[2])
def ev(d):
    body = "".join(f"{k}: {urllib.parse.quote(str(v))}\n" for k, v in d.items()) + "\n"
    return f"Content-Length: {len(body)}\nContent-Type: text/event-plain\n\n{body}".encode()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(1)
c, _ = s.accept(); f = c.makefile("rb")
cmds = []
lock = threading.Lock()
def send(b):
    with lock: c.sendall(b)
def reader():
    buf = []
    for raw in f:
        l = raw.decode().rstrip("\n")
        if l == "":
            if buf: cmds.append(" ".join(buf)); send(b"Content-Type: command/reply\nReply-Text: +OK\n\n")
            buf = []
        else: buf.append(l)
c.sendall(b"Content-Type: auth/request\n\n")
threading.Thread(target=reader, daemon=True).start()
while len(cmds) < 5: time.sleep(0.01)   # auth, event, 3 filters
t0 = time.time(); out = bytearray()
for i in range(N):
    u = f"n{port}-{i}"; pol = "vmdrop" if i % 2 else "agent"
    out += ev({"Event-Name": "CUSTOM", "Event-Subclass": "amvd::result", "Unique-ID": u, "AMVD-Result": "machine" if i % 2 else "human",
               "AMVD-Reason": "x", "AMVD-Decision-MS": "2000", "variable_amvd_policy": pol, "AMVD-Features": "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"})
    if i % 2: out += ev({"Event-Name": "CUSTOM", "Event-Subclass": "amvd::message_end", "Unique-ID": u, "AMVD-End-Type": "beep", "AMVD-End-MS": "6000"})
    out += ev({"Event-Name": "CHANNEL_HANGUP_COMPLETE", "Unique-ID": u, "Hangup-Cause": "NORMAL_CLEARING", "variable_billsec": "30"})
    if len(out) > 1 << 16: send(bytes(out)); out = bytearray()
send(bytes(out))
time.sleep(1.5)
print(f"node {port}: sent {N} calls in {time.time()-t0:.2f}s, received {len(cmds)-5} commands", flush=True)
