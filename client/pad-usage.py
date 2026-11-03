#!/usr/bin/env python3
"""Hook Claude Code : envoie au Claude Pad la consommation de la session.

Lit le transcript (transcript_path) et calcule :
  - le contexte occupé (dernier appel : entrée + cache lu + cache écrit),
  - les tokens cumulés (entrée, sortie, cache lu, cache écrit),
  - le prix équivalent API (sous-agents compris).

Branché sur PostToolUse et Stop, en hook « async » : Claude Code le lance
en arrière-plan, il ne ralentit jamais Claude.

Adresse du pad : --pad URL, sinon CLAUDE_PAD_URL, sinon http://claude-pad.local
"""

import glob
import json
import os
import sys
import urllib.request

if "--pad" in sys.argv[1:-1]:
    PAD = sys.argv[sys.argv.index("--pad") + 1].rstrip("/")
else:
    PAD = os.environ.get("CLAUDE_PAD_URL", "http://claude-pad.local").rstrip("/")

# $ par million de tokens : entrée, sortie, cache lu. Écriture du cache :
# 1,25 x l'entrée (5 min) ou 2 x (1 h). Tarifs API Anthropic, octobre 2026.
PRICES = {
    "claude-fable-5-1":  (10.00, 50.00, 0.25),
    "claude-fable-5":    (10.00, 50.00, 1.00),
    "claude-opus-5-5":   (4.00, 20.00, 0.20),
    "claude-opus-5":     (5.00, 25.00, 0.50),
    "claude-opus-4":     (5.00, 25.00, 0.50),
    "claude-sonnet-5-5": (2.00, 10.00, 0.20),
    "claude-sonnet-5":   (2.00, 10.00, 0.20),
    "claude-sonnet-4":   (3.00, 15.00, 0.30),
    "claude-haiku-5-5":  (0.10, 0.50, 0.01),
    "claude-haiku-4":    (1.00, 5.00, 0.10),
}
NAMES = {"fable": "Fable", "opus": "Opus", "sonnet": "Sonnet", "haiku": "Haiku"}


def price_for(model):
    """Tarif du préfixe le plus long qui correspond (claude-opus-4-8 -> claude-opus-4)."""
    best = None
    for key in PRICES:
        if model.startswith(key) and (best is None or len(key) > len(best)):
            best = key
    return PRICES.get(best)


def pretty(model):
    """claude-opus-5-5 -> Opus 5.5"""
    parts = model.replace("[1m]", "").split("-")
    if len(parts) < 3 or parts[0] != "claude":
        return model
    return NAMES.get(parts[1], parts[1]) + " " + ".".join(p for p in parts[2:] if len(p) <= 2)


def window_for(model):
    return 200_000 if model.startswith("claude-haiku-4") else 1_000_000


def read_usage(path, totals):
    """Ajoute à `totals` les appels d'un transcript. Renvoie le dernier appel."""
    calls = {}  # un message est écrit sur plusieurs lignes : on garde la dernière
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if '"usage"' not in line:
                    continue
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                m = d.get("message") or {}
                if d.get("type") == "assistant" and m.get("usage") and m.get("model", "").startswith("claude"):
                    calls[m.get("id") or len(calls)] = m
    except OSError:
        return None

    last = None
    for m in calls.values():
        u = m["usage"]
        cc = u.get("cache_creation") or {}
        w5 = cc.get("ephemeral_5m_input_tokens", 0) or 0
        w1h = cc.get("ephemeral_1h_input_tokens", 0) or 0
        wr = u.get("cache_creation_input_tokens", 0) or 0
        if not cc:
            w5 = wr
        inp = u.get("input_tokens", 0) or 0
        out = u.get("output_tokens", 0) or 0
        rd = u.get("cache_read_input_tokens", 0) or 0

        totals["input"] += inp
        totals["output"] += out
        totals["cache_read"] += rd
        totals["cache_write"] += wr
        p = price_for(m["model"])
        if p:
            k = 2 if u.get("speed") == "fast" else 1
            totals["cost"] += k * (inp * p[0] + out * p[1] + rd * p[2]
                                   + w5 * p[0] * 1.25 + w1h * p[0] * 2) / 1e6
        last = m
    return last


def send(session_id, cwd, path):
    """Calcule la consommation et l'envoie au pad."""
    totals = {"input": 0, "output": 0, "cache_read": 0, "cache_write": 0, "cost": 0.0}
    last = read_usage(path, totals)
    if last is None:
        return
    # Sous-agents : transcripts à part dans <session>/subagents/
    for sub in glob.glob(os.path.join(path[:-len(".jsonl")], "subagents", "*.jsonl")):
        read_usage(sub, totals)

    u = last["usage"]
    model = last["model"]
    body = dict(totals,
                session_id=session_id,
                cwd=cwd,
                model=pretty(model),
                context=(u.get("input_tokens", 0) or 0) + (u.get("cache_read_input_tokens", 0) or 0)
                        + (u.get("cache_creation_input_tokens", 0) or 0),
                window=window_for(model))
    body["cost"] = round(body["cost"], 4)
    req = urllib.request.Request(PAD + "/usage", data=json.dumps(body).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=3).read()
    except Exception:
        pass


def main():
    try:
        event = json.load(sys.stdin)
    except Exception:
        return
    if event.get("transcript_path"):
        send(event.get("session_id", ""), event.get("cwd", ""), event["transcript_path"])


if __name__ == "__main__":
    main()
