#!/usr/bin/env python3
"""
Minimal SIP UAS that answers every INVITE and streams a corpus file to the caller as
real PCMU RTP (20 ms packets, wall-clock paced) - i.e. what a carrier delivers to the
caller leg where mod_amvd listens.

The file is chosen by the X-Wav SIP header (originate with sip_h_X-Wav=<name>.wav);
the matching <name>.PCMU must exist in --audio-dir (make them with to_pcmu.py).

Answer timing is chosen by the X-Ring header (sip_h_X-Ring=<code>:<ms>): send 180 (ringing)
or 183 (session progress with SDP = early media) and wait <ms> before the 200 OK.
Without it the callee answers at once with no provisional response, as a FAS carrier does.

Test tool only: no auth, no retransmissions, 127.0.0.1.
"""
import argparse
import random
import re
import socket
import struct
import threading
import time


def header(msg, name):
    out = []
    for line in msg.split("\r\n"):
        if re.match(r"(?:%s)\s*:" % name, line, re.I):
            out.append(line.split(":", 1)[1].strip())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--audio-dir", required=True, help="directory of .PCMU files")
    ap.add_argument("--port", type=int, default=15090)
    ap.add_argument("--rtp-base", type=int, default=41000)
    args = ap.parse_args()

    sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sip.bind(("127.0.0.1", args.port))
    calls = {}
    next_port = [args.rtp_base]

    def stream(call_id, name, rip, rport, lport):
        try:
            data = open("%s/%s.PCMU" % (args.audio_dir, name), "rb").read()
        except OSError as e:
            print("missing audio:", e, flush=True)
            data = b""
        rs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rs.bind(("127.0.0.1", lport))
        seq, ts, ssrc = random.randint(0, 60000), random.randint(0, 2**31), random.randint(1, 2**31)
        t = time.time()
        for i in range(0, len(data), 160):
            if call_id not in calls:
                break
            payload = data[i:i + 160].ljust(160, b"\xff")
            rs.sendto(struct.pack("!BBHII", 0x80, 0, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc) + payload, (rip, rport))
            seq += 1
            ts += 160
            t += 0.02
            delay = t - time.time()
            if delay > 0:
                time.sleep(delay)
        calls.pop(call_id, None)
        rs.close()

    print("sip_callee listening on 127.0.0.1:%d" % args.port, flush=True)
    while True:
        raw, addr = sip.recvfrom(65535)
        msg = raw.decode(errors="replace")
        first = msg.split("\r\n", 1)[0]

        def reply(code, text, extra="", body="", tag=True):
            to = header(msg, "To")[0]
            if tag and "tag=" not in to:
                to += ";tag=uas%d" % (hash(header(msg, "Call-ID")[0]) % 99999)
            vias = "\r\n".join("Via: " + v for v in header(msg, "Via"))
            out = ("SIP/2.0 %d %s\r\n%s\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n%s"
                   "Content-Length: %d\r\n\r\n%s") % (code, text, vias, header(msg, "From")[0], to,
                                                      header(msg, "Call-ID")[0], header(msg, "CSeq")[0],
                                                      extra, len(body), body)
            sip.sendto(out.encode(), addr)

        if first.startswith("INVITE"):
            call_id = header(msg, "Call-ID")[0]
            wav = (header(msg, "X-Wav") or [""])[0]
            name = wav.rsplit("/", 1)[-1].rsplit(".", 1)[0]
            sdp = msg.split("\r\n\r\n", 1)[1]
            rip = re.search(r"c=IN IP4 (\S+)", sdp).group(1)
            rport = int(re.search(r"m=audio (\d+)", sdp).group(1))
            next_port[0] += 2
            lport = next_port[0]
            if next_port[0] > args.rtp_base + 4000:
                next_port[0] = args.rtp_base
            answer = ("v=0\r\no=uas 1 1 IN IP4 127.0.0.1\r\ns=-\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                      "m=audio %d RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\na=ptime:20\r\na=sendrecv\r\n") % lport
            reply(100, "Trying", tag=False)
            calls[call_id] = True
            ring = (header(msg, "X-Ring") or [""])[0]
            code, _, ms = ring.partition(":")
            sdp_hdr = "Contact: <sip:uas@127.0.0.1:%d>\r\nContent-Type: application/sdp\r\n" % args.port

            def answer_call(reply=reply, call_id=call_id, name=name, rip=rip, rport=rport, lport=lport,
                            answer=answer, code=code, ms=ms):
                if code == "180":
                    reply(180, "Ringing", "Contact: <sip:uas@127.0.0.1:%d>\r\n" % args.port)
                elif code == "183":
                    reply(183, "Session Progress", sdp_hdr, answer)
                if ms:
                    time.sleep(int(ms) / 1000.0)
                if call_id not in calls:  # cancelled while ringing
                    return
                reply(200, "OK", sdp_hdr, answer)
                stream(call_id, name, rip, rport, lport)

            threading.Thread(target=answer_call, daemon=True).start()
        elif first.startswith("BYE"):
            calls.pop(header(msg, "Call-ID")[0], None)
            reply(200, "OK", tag=False)
        elif first.startswith("CANCEL"):
            calls.pop(header(msg, "Call-ID")[0], None)
            reply(200, "OK", tag=False)


if __name__ == "__main__":
    main()
