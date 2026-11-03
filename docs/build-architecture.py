#!/usr/bin/env python3
"""Régénère docs/architecture.html à partir des blocs mermaid du README.md.

    python3 docs/build-architecture.py

Le titre de chaque diagramme est la dernière ligne de texte qui le précède
dans le README (sans le « : » final).
"""

import html
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
OUT = ROOT / "docs" / "architecture.html"

PAGE = """<!doctype html>
<html lang="fr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Pad — architecture</title>
<style>body{{font-family:-apple-system,system-ui,sans-serif;margin:24px;background:#fff;color:#1c1d24}}h1{{font-size:22px}}h2{{font-size:17px;margin-top:32px}}pre.mermaid{{background:none}}</style>
</head><body>
<h1>Claude Pad — architecture</h1>
<!-- Généré par docs/build-architecture.py depuis les blocs mermaid du README.md -->
{body}
<script type="module">
import mermaid from "https://cdn.jsdelivr.net/npm/mermaid@11.4.1/dist/mermaid.esm.min.mjs";
mermaid.initialize({{startOnLoad: true}});
</script>
</body></html>
"""


def title_before(text, pos, n):
    """Dernière ligne de texte avant `pos`, hors titres Markdown et liens."""
    for line in reversed(text[:pos].splitlines()):
        line = line.strip()
        if line and not line.startswith(("#", "```", "Rendu hors GitHub")):
            return html.escape(line.rstrip(" :"))
        if line.startswith("## "):
            return html.escape(line[3:])
    return f"Diagramme {n}"


def main():
    text = README.read_text(encoding="utf-8")
    parts = []
    for n, m in enumerate(re.finditer(r"```mermaid\n(.*?)```", text, re.S), 1):
        title = "Vue d'ensemble" if n == 1 else title_before(text, m.start(), n)
        parts.append(f'<h2>{title}</h2><pre class="mermaid">{html.escape(m.group(1))}</pre>')
    OUT.write_text(PAGE.format(body="".join(parts)), encoding="utf-8")
    print(f"{OUT.relative_to(ROOT)} : {len(parts)} diagramme(s)")


if __name__ == "__main__":
    main()
