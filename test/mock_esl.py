#!/usr/bin/env python3
"""Mock FreeSWITCH event socket: auth, emits amvd events for 3 calls, records bgapi commands."""
import socket, sys, time, urllib.parse
def ev(d):
    body = "".join(f"{k}: {urllib.parse.quote(str(v))}\n" for k, v in d.items()) + "\n"
    return f"Content-Length: {len(body)}\nContent-Type: text/event-plain\n\n{body}".encode()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 18021)); s.listen(1)
c, _ = s.accept(); f = c.makefile("rb")
def readcmd():
    lines = []
    while True:
        l = f.readline().decode().rstrip("\n")
        if l == "": return " ".join(lines)
        lines.append(l)
c.sendall(b"Content-Type: auth/request\n\n")
print("CMD", readcmd()); c.sendall(b"Content-Type: command/reply\nReply-Text: +OK accepted\n\n")
print("CMD", readcmd()); c.sendall(b"Content-Type: command/reply\nReply-Text: +OK event listener enabled plain\n\n")
base = {"Event-Name": "CUSTOM", "AMVD-Profile": "ai-agent", "AMVD-Model": "rules", "AMVD-Features": "540,600,0.3"}
c.sendall(ev({**base, "Event-Subclass": "amvd::result", "Unique-ID": "u-human", "AMVD-Result": "human", "AMVD-Reason": "short_greeting", "AMVD-Decision-MS": "1900", "variable_amvd_policy": "agent"}))
c.sendall(ev({**base, "Event-Subclass": "amvd::result", "Unique-ID": "u-vm", "AMVD-Result": "machine", "AMVD-Reason": "long_greeting", "AMVD-Decision-MS": "3300", "variable_amvd_policy": "vmdrop", "variable_amvd_campaign": "42"}))
c.sendall(ev({**base, "Event-Subclass": "amvd::result", "Unique-ID": "u-fax", "AMVD-Result": "fax", "AMVD-Reason": "fax_ced", "AMVD-Decision-MS": "800"}))
time.sleep(0.3)
c.sendall(ev({**base, "Event-Subclass": "amvd::message_end", "Unique-ID": "u-vm", "AMVD-End-Type": "beep", "AMVD-End-MS": "7400", "AMVD-Beep-Hz": "1000"}))
time.sleep(0.6)  # leave time for the transcript POST
for u in ("u-human", "u-vm", "u-fax"):
    c.sendall(ev({"Event-Name": "CHANNEL_HANGUP_COMPLETE", "Unique-ID": u, "Hangup-Cause": "NORMAL_CLEARING", "variable_billsec": "12"}))
c.settimeout(1.0)
try:
    while True:
        cmd = readcmd()
        if not cmd: break
        print("CMD", cmd)
except Exception: pass
