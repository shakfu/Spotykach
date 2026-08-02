#!/usr/bin/env python3
"""Markdown to HTML, for the engine documentation the web front-end renders in place.

**This is not a CommonMark implementation and does not try to be.** It converts the subset that
`docs/engines/*.md` actually uses, which a survey of those files pins down exactly: ATX headings,
paragraphs, fenced code, pipe tables, unordered and ordered lists, blockquotes, horizontal rules,
images, links, bold, italic and inline code. Anything outside that is passed through as escaped text
rather than guessed at, so an unsupported construct looks plain instead of looking broken.

Why hand-rolled rather than a library: the card tooling is stdlib-only so `make dist` works with no
venv, and this runs beside it in `web_export.py`. The alternative - shipping a markdown parser to the
browser - is a runtime dependency for a page that has none, and it would parse the same fixed set of
files on every visit instead of once at export.

SAFETY. Every piece of source text is HTML-escaped before any tag is inserted, so a stray `<script>`
in a doc becomes visible text rather than a script. That matters because the output is injected with
`innerHTML`: the content is ours, generated at build time from files in this repo, but "ours" is a
policy and escaping is a mechanism. `test_md2html.py` asserts no raw tag survives from source.
"""

from __future__ import annotations

import html
import re
from typing import Callable

# A link rewriter maps a markdown href to whatever the page should actually point at. Injected
# because the answer differs per destination: a sibling doc is not shipped and has to go to GitHub,
# a diagram IS shipped and has to stay local.
Rewriter = Callable[[str], str]


def _esc(text: str) -> str:
    return html.escape(text, quote=False)


# Schemes an href may use. Everything else - `javascript:`, `data:`, `vbscript:` - is dropped, and a
# relative path (no scheme at all) is fine. Found by feeding the converter a deliberately hostile
# document: `[x](javascript:alert(1))` produced a live handler, and while nothing in docs/engines/
# contains one, a generator that CAN emit it is a generator that will the day somebody pastes a link.
_SAFE_SCHEME = re.compile(r"^(?:https?:|mailto:|#|[^a-z]|[a-z][\w+.-]*[^:\w+.-])", re.I)


def _safe_href(href: str) -> str:
    stripped = href.strip()
    if ":" not in stripped.split("/")[0]:
        return stripped  # relative path or fragment - no scheme to worry about
    if _SAFE_SCHEME.match(stripped):
        return stripped
    return "#"


_CODE_SPAN = re.compile(r"`([^`]+)`")
_IMAGE = re.compile(r"!\[([^\]]*)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
_LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
_BOLD = re.compile(r"\*\*([^*]+)\*\*")
_ITALIC = re.compile(r"(?<![*\w])\*([^*\n]+)\*(?!\*)")


def inline(text: str, rewrite: Rewriter) -> str:
    """Inline markup for one run of text.

    Code spans are lifted out to placeholders rather than split on, and that detail is the whole
    reason this function is written the way it is. Splitting leaves the surrounding text in separate
    pieces, so `**a `b` c**` has its two asterisk pairs in different fragments and neither matches -
    which silently printed 72 literal `**` across these docs. Placeholders keep the run contiguous
    while still protecting the code, where `**` is two asterisks and a path is not a link.
    """
    spans: list[str] = []

    def stash(m: re.Match[str]) -> str:
        spans.append(_esc(m.group(1)))
        return f"\x00{len(spans) - 1}\x00"

    s = _CODE_SPAN.sub(stash, text)
    s = _esc(s)  # NUL placeholders pass through escaping untouched
    # Images before links: `![a](b)` contains `[a](b)`, so the link rule would eat it first.
    s = _IMAGE.sub(lambda m: f'<img src="{html.escape(_safe_href(rewrite(m.group(2))))}" '
                             f'alt="{html.escape(m.group(1))}" loading="lazy">', s)
    s = _LINK.sub(
        lambda m: f'<a href="{html.escape(_safe_href(rewrite(m.group(2))))}">{m.group(1)}</a>', s)
    s = _BOLD.sub(r"<strong>\1</strong>", s)
    s = _ITALIC.sub(r"<em>\1</em>", s)
    return re.sub(r"\x00(\d+)\x00", lambda m: f"<code>{spans[int(m.group(1))]}</code>", s)


_HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*$")
_HR = re.compile(r"^\s*(?:-{3,}|\*{3,}|_{3,})\s*$")
_ULI = re.compile(r"^(\s*)[-*]\s+(.*)$")
_OLI = re.compile(r"^(\s*)\d+\.\s+(.*)$")
_TABLE_RULE = re.compile(r"^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$")


def _row(line: str) -> list[str]:
    """Split a pipe-table row. Leading and trailing pipes are optional, as they are in the docs."""
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|"):
        s = s[:-1]
    return [c.strip() for c in s.split("|")]


def convert(md: str, rewrite: Rewriter = lambda href: href, *, skip_h1: bool = False) -> str:
    """Convert a markdown document to an HTML fragment (no <html>, no <body>).

    `skip_h1` drops the document's leading `# Title`, which the page renders itself as the engine's
    heading - keeping it would print the name twice.
    """
    lines = md.replace("\r\n", "\n").split("\n")
    out: list[str] = []
    i = 0
    seen_h1 = False

    while i < len(lines):
        line = lines[i]

        # --- fenced code ---------------------------------------------------------------------
        if line.lstrip().startswith("```"):
            lang = line.lstrip()[3:].strip()
            i += 1
            buf: list[str] = []
            while i < len(lines) and not lines[i].lstrip().startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1  # the closing fence
            cls = f' class="lang-{html.escape(lang)}"' if lang else ""
            out.append(f"<pre><code{cls}>{_esc(chr(10).join(buf))}</code></pre>")
            continue

        # --- headings ------------------------------------------------------------------------
        m = _HEADING.match(line)
        if m:
            level = len(m.group(1))
            if level == 1 and skip_h1 and not seen_h1:
                seen_h1 = True
                i += 1
                continue
            # Headings shift down one level: the page supplies the h1, so a doc's h1 becomes an h2
            # and the document stays a single well-formed outline rather than two competing ones.
            shifted = min(level + 1, 6)
            out.append(f"<h{shifted}>{inline(m.group(2), rewrite)}</h{shifted}>")
            i += 1
            continue

        # --- horizontal rule -----------------------------------------------------------------
        if _HR.match(line):
            out.append("<hr>")
            i += 1
            continue

        # --- table ---------------------------------------------------------------------------
        if line.strip().startswith("|") and i + 1 < len(lines) and _TABLE_RULE.match(lines[i + 1]):
            head = _row(line)
            i += 2
            body: list[list[str]] = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                body.append(_row(lines[i]))
                i += 1
            cells = "".join(f"<th>{inline(c, rewrite)}</th>" for c in head)
            rows = "".join(
                "<tr>" + "".join(f"<td>{inline(c, rewrite)}</td>" for c in r) + "</tr>"
                for r in body)
            out.append(f"<table><thead><tr>{cells}</tr></thead><tbody>{rows}</tbody></table>")
            continue

        # --- blockquote ----------------------------------------------------------------------
        if line.lstrip().startswith(">"):
            buf = []
            while i < len(lines) and lines[i].lstrip().startswith(">"):
                buf.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            out.append(f"<blockquote>{convert(chr(10).join(buf), rewrite)}</blockquote>")
            continue

        # --- lists ---------------------------------------------------------------------------
        if _ULI.match(line) or _OLI.match(line):
            ordered = bool(_OLI.match(line))
            pattern = _OLI if ordered else _ULI
            items: list[str] = []
            while i < len(lines):
                m = pattern.match(lines[i])
                if not m:
                    # A continuation line belongs to the item above it, not to a new paragraph.
                    if items and lines[i].startswith(("  ", "\t")) and lines[i].strip():
                        items[-1] += " " + inline(lines[i].strip(), rewrite)
                        i += 1
                        continue
                    break
                items.append(inline(m.group(2), rewrite))
                i += 1
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>" + "".join(f"<li>{it}</li>" for it in items) + f"</{tag}>")
            continue

        # --- blank ---------------------------------------------------------------------------
        if not line.strip():
            i += 1
            continue

        # --- paragraph -----------------------------------------------------------------------
        buf = []
        while i < len(lines) and lines[i].strip() and not _is_block_start(lines[i], lines, i):
            buf.append(lines[i].strip())
            i += 1
        text = " ".join(buf)
        # A paragraph that is nothing but an image is a figure, not a sentence with a picture in it.
        only_image = _IMAGE.fullmatch(text.strip())
        if only_image:
            # Linked to itself, deliberately. The control diagrams are ~4000 units wide, so at any
            # column width the page can offer they scale to about a third and their labels become
            # illegible - the picture is visible and useless. The inline copy is the thumbnail; the
            # link is how you actually read it.
            src = html.escape(_safe_href(rewrite(only_image.group(2))))
            alt = _esc(only_image.group(1)) or "Diagram"
            out.append(
                f'<figure><a href="{src}" target="_blank" rel="noreferrer">'
                f"{inline(text, rewrite)}</a>"
                f"<figcaption>{alt} - open full size</figcaption></figure>")
        else:
            out.append(f"<p>{inline(text, rewrite)}</p>")

    return "\n".join(out)


def _is_block_start(line: str, lines: list[str], i: int) -> bool:
    """Would this line begin a new block? Stops a paragraph from swallowing the table below it."""
    if _HEADING.match(line) or _HR.match(line) or _ULI.match(line) or _OLI.match(line):
        return True
    if line.lstrip().startswith((">", "```")):
        return True
    if line.strip().startswith("|") and i + 1 < len(lines) and _TABLE_RULE.match(lines[i + 1]):
        return True
    return False
