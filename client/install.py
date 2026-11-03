#!/usr/bin/env python3
"""Branche Claude Code sur le Claude Pad (macOS, Linux, Windows).

    python3 install.py [adresse du pad]    défaut : http://claude-pad.local
    python3 install.py --uninstall         retire les hooks du pad
    (Windows : py install.py 192.168.1.169)

Ajoute les hooks du pad dans ~/.claude/settings.json (CLAUDE_SETTINGS pour
un autre fichier) sans toucher au reste. Relancer le script remplace les
hooks du pad déjà présents : pratique pour changer d'adresse.
Une copie de sauvegarde est faite à côté (settings.json.bak).
"""

import json
import os
import pathlib
import shlex
import shutil
import sys
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = ("pad-event.py", "pad-prompt.py", "pad-usage.py")


def python():
    """Python qui lancera les hooks. Sur macOS / Linux, celui du système de
    préférence : le python3 du PATH peut être celui d'un environnement
    virtuel (PlatformIO, projet...) qui risque de disparaître."""
    if os.name != "nt" and os.path.exists("/usr/bin/python3"):
        return "/usr/bin/python3"
    return sys.executable


def quote(arg):
    """Argument de commande, pour le shell qui lancera le hook.

    Windows : guillemets doubles et barres obliques, compris aussi bien par
    cmd et PowerShell que par Git Bash (qui mangerait les \\ hors guillemets).
    """
    if os.name == "nt":
        return '"' + arg.replace("\\", "/") + '"'
    return shlex.quote(arg)


def is_ours(hook):
    """Hook du pad, y compris les anciennes versions (curl ... /event)."""
    c = hook.get("command", "")
    return any(s in c for s in SCRIPTS) or ("curl" in c and "/event" in c)


def remove_ours(hooks):
    for event in list(hooks):
        entries = []
        for e in hooks[event]:
            e["hooks"] = [h for h in e.get("hooks", []) if not is_ours(h)]
            if e["hooks"]:
                entries.append(e)
        if entries:
            hooks[event] = entries
        else:
            del hooks[event]


def add_ours(hooks, pad):
    def cmd(script, timeout=None):
        h = {"type": "command",
             "command": " ".join(quote(a) for a in (python(), str(HERE / script), "--pad", pad))}
        if timeout:
            h["timeout"] = timeout
        return h

    event, prompt, usage = cmd("pad-event.py"), cmd("pad-prompt.py", 120), cmd("pad-usage.py")
    add = {
        "SessionStart":      [{"hooks": [event]}],
        "UserPromptSubmit":  [{"hooks": [event]}],
        "PreToolUse":        [{"matcher": "*", "hooks": [event]},
                              {"matcher": "AskUserQuestion|ExitPlanMode", "hooks": [prompt]}],
        "PostToolUse":       [{"matcher": "*", "hooks": [event, usage]}],
        "PermissionRequest": [{"matcher": "*", "hooks": [prompt]}],
        "Notification":      [{"hooks": [event]}],
        "Stop":              [{"hooks": [event, usage]}],
        "SessionEnd":        [{"hooks": [event]}],
    }
    for name, entries in add.items():
        hooks.setdefault(name, []).extend(entries)


def main():
    args = sys.argv[1:]
    uninstall = "--uninstall" in args
    pad = ""
    if not uninstall:
        pad = (args[0] if args else "http://claude-pad.local").rstrip("/")
        if not pad.startswith(("http://", "https://")):
            pad = "http://" + pad
        try:
            urllib.request.urlopen(pad + "/", timeout=3).read()
            print("Pad joignable :", pad)
        except Exception:
            print(f"Attention : pas de réponse de {pad} (pad éteint ?). Les hooks sont installés quand même.")

    path = pathlib.Path(os.environ.get("CLAUDE_SETTINGS") or pathlib.Path.home() / ".claude" / "settings.json")
    path.parent.mkdir(parents=True, exist_ok=True)
    settings, backup = {}, ""
    if path.exists():
        settings = json.loads(path.read_text(encoding="utf-8") or "{}")
        shutil.copy2(path, str(path) + ".bak")
        backup = f" (sauvegarde : {path}.bak)"

    hooks = settings.get("hooks", {})
    remove_ours(hooks)
    if pad:
        add_ours(hooks, pad)
    if hooks:
        settings["hooks"] = hooks
    else:
        settings.pop("hooks", None)
    path.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    if os.name != "nt":
        for s in SCRIPTS:
            (HERE / s).chmod(0o755)
    print(f"Hooks du pad {'retirés de' if uninstall else 'installés dans'} {path}{backup}.")


if __name__ == "__main__":
    main()
