#!/usr/bin/env python3
"""An LXMF peer for benching SolarOS over LoRa.

    rns_lora_bench_peer.py <configdir> <peer delivery hash> [text]

Announces as "mac" with an identity kept in <configdir>/mac_identity, sends
<text> to the peer if given, and keeps running, printing every message it
receives and when a delivery proof comes back.
"""
import os, sys, time, RNS, LXMF

cfg, peer = sys.argv[1], bytes.fromhex(sys.argv[2])
text = sys.argv[3] if len(sys.argv) > 3 else None
RNS.Reticulum(cfg)
router = LXMF.LXMRouter(storagepath=os.path.join(cfg, "lxmf"))
path = os.path.join(cfg, "mac_identity")
ident = RNS.Identity.from_file(path) if os.path.exists(path) else None
if ident is None:
    ident = RNS.Identity(); ident.to_file(path)
me = router.register_delivery_identity(ident, display_name="mac")
def stamp(): return time.strftime("%H:%M:%S")
router.register_delivery_callback(lambda m: print(stamp(), "RECEIVED from", RNS.prettyhexrep(m.source_hash), "content", repr(m.content_as_string()), flush=True))
print("mac delivery", RNS.prettyhexrep(me.hash), flush=True)
router.announce(me.hash); print("announced", flush=True)
if text:
    time.sleep(8)
    if not RNS.Transport.has_path(peer):
        RNS.Transport.request_path(peer)
        for _ in range(30):
            if RNS.Transport.has_path(peer): break
            time.sleep(1)
    dest_ident = RNS.Identity.recall(peer)
    print("path:", RNS.Transport.has_path(peer), "identity:", dest_ident is not None, flush=True)
    if dest_ident is not None:
        dst = RNS.Destination(dest_ident, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")
        msg = LXMF.LXMessage(dst, me, text, desired_method=LXMF.LXMessage.OPPORTUNISTIC)
        msg.register_delivery_callback(lambda m: print(stamp(), "DELIVERED (proof received)", flush=True))
        msg.register_failed_callback(lambda m: print(stamp(), "FAILED", flush=True))
        router.handle_outbound(msg); print(stamp(), "sent", repr(text), flush=True)
while True: time.sleep(1)
