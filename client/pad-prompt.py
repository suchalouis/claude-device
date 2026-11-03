#!/usr/bin/env python3
"""Hook Claude Code : fait répondre le Claude Pad à la place du menu.

Branché sur PermissionRequest (permissions) et sur PreToolUse pour
AskUserQuestion / ExitPlanMode (questions, plan). Envoie la demande au pad,
attend le choix touché à l'écran, puis renvoie la décision à Claude Code.

Pad absent, délai dépassé ou demande répondue sur le PC : le script ne
renvoie rien et Claude Code affiche son menu habituel. Il ne bloque jamais.

Adresse du pad : --pad URL, sinon CLAUDE_PAD_URL, sinon http://claude-pad.local
CLAUDE_PAD_TIMEOUT : secondes d'attente avant de rendre la main (défaut 90)
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid

if "--pad" in sys.argv[1:-1]:
    PAD = sys.argv[sys.argv.index("--pad") + 1].rstrip("/")
else:
    PAD = os.environ.get("CLAUDE_PAD_URL", "http://claude-pad.local").rstrip("/")
TIMEOUT = float(os.environ.get("CLAUDE_PAD_TIMEOUT", "90"))
POLL = 0.3
REASON = "Refusé depuis le Claude Pad"


def call(method, path, body=None, timeout=2.0):
    """Renvoie (statut HTTP, corps) ; lève une exception si le pad est injoignable."""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(PAD + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, ""


def wait_for_answer(event):
    """Envoie la demande au pad et attend le choix. None = rendre la main."""
    pid = uuid.uuid4().hex[:12]
    try:
        status, _ = call("POST", "/prompt?id=" + pid, event)
    except Exception:
        return None  # pad éteint ou injoignable
    if status != 200:
        return None

    deadline = time.time() + TIMEOUT
    while time.time() < deadline:
        time.sleep(POLL)
        try:
            status, body = call("GET", "/answer?id=" + pid)
        except Exception:
            continue  # coupure Wi-Fi passagère
        if status == 200:
            return json.loads(body)
        if status != 202:
            return None  # demande close côté pad (répondue sur le PC...)

    try:
        call("POST", "/cancel?id=" + pid)
    except Exception:
        pass
    return None


def permission_output(event, answer):
    decision = answer.get("decision")
    if decision == "deny":
        out = {"behavior": "deny", "message": REASON}
    else:
        out = {"behavior": "allow"}
        if decision == "always":
            rules = event.get("permission_suggestions") or []
            if not rules and event.get("tool_name") in ("Edit", "Write", "MultiEdit", "NotebookEdit"):
                # Pas de suggestion pour les modifs de fichiers : on accepte
                # les modifications pour le reste de la session
                rules = [{"type": "setMode", "mode": "acceptEdits", "destination": "session"}]
            if rules:
                out["updatedPermissions"] = rules
    return {"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": out}}


def pretooluse_output(event, answer):
    if answer.get("decision") == "deny":
        return {"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                       "permissionDecision": "deny",
                                       "permissionDecisionReason": REASON}}
    tool_input = dict(event.get("tool_input") or {})
    if event.get("tool_name") == "AskUserQuestion":
        questions = tool_input.get("questions") or []
        answers = answer.get("answers") or []
        tool_input["answers"] = {q.get("question", ""): a for q, a in zip(questions, answers)}
    return {"hookSpecificOutput": {"hookEventName": "PreToolUse",
                                   "permissionDecision": "allow",
                                   "updatedInput": tool_input}}


def main():
    try:
        event = json.load(sys.stdin)
    except Exception:
        return
    name = event.get("hook_event_name")
    tool = event.get("tool_name")
    interactive = tool in ("AskUserQuestion", "ExitPlanMode")

    # Questions et plan passent par PreToolUse ; s'ils arrivent aussi en
    # PermissionRequest (pad ignoré ou délai dépassé), on laisse le menu.
    if name == "PermissionRequest" and interactive:
        return
    if name == "PreToolUse" and not interactive:
        return
    if name not in ("PermissionRequest", "PreToolUse"):
        return

    answer = wait_for_answer(event)
    if answer is None:
        return
    if name == "PermissionRequest":
        print(json.dumps(permission_output(event, answer)))
    else:
        print(json.dumps(pretooluse_output(event, answer)))


if __name__ == "__main__":
    main()
