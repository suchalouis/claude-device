#!/usr/bin/env python3
"""Hook Claude Code : transmet l'événement au Claude Pad (affichage).

    python3 pad-event.py --pad http://192.168.1.169

Remplace `curl ... /event` : même effet, mais marche aussi sous Windows.
Pad éteint ou injoignable : on abandonne après 1 s, Claude n'est jamais bloqué.
"""

import os
import sys
import urllib.request


def pad_url():
    """--pad URL, sinon CLAUDE_PAD_URL, sinon http://claude-pad.local"""
    if "--pad" in sys.argv[1:-1]:
        return sys.argv[sys.argv.index("--pad") + 1].rstrip("/")
    return os.environ.get("CLAUDE_PAD_URL", "http://claude-pad.local").rstrip("/")


def main():
    body = sys.stdin.buffer.read()
    req = urllib.request.Request(pad_url() + "/event", data=body, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=1).read()
    except Exception:
        pass


if __name__ == "__main__":
    main()
