"""Tests for the markdown subset converter that renders the engine docs.

Two jobs. First, the constructs `docs/engines/*.md` actually use have to survive conversion - a table
that comes out as literal pipes is a page nobody can read. Second, and more important, the output is
injected into the page with `innerHTML`, so nothing in a source document may become live markup.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import md2html  # noqa: E402

DOCS = Path(__file__).resolve().parent.parent / "docs" / "engines"


def convert(md, *args, **kw):
    return md2html.convert(md, *args, **kw)


# --- the subset the docs use ---------------------------------------------------------------------


def test_headings_shift_down_one_level():
    # The page supplies the h1, so a doc's own h1 becomes an h2 and the outline stays single.
    assert "<h2>Title</h2>" in convert("# Title")
    assert "<h3>Sub</h3>" in convert("## Sub")
    assert "<h6>Deep</h6>" in convert("###### Deep")


def test_skip_h1_drops_only_the_first():
    out = convert("# Name\n\ntext\n\n# Later\n", skip_h1=True)
    assert "Name" not in out
    assert "<h2>Later</h2>" in out


def test_table_becomes_a_table():
    out = convert("| a | b |\n|---|---|\n| 1 | 2 |\n")
    assert "<table>" in out and "<th>a</th>" in out and "<td>2</td>" in out
    assert "|" not in re.sub(r"<[^>]+>", "", out)


def test_fenced_code_is_escaped_and_not_reparsed():
    out = convert("```sh\nmake ENGINE=x  # **not bold**\n```")
    assert '<pre><code class="lang-sh">' in out
    assert "**not bold**" in out, "markdown inside a fence must stay literal"


def test_inline_code_protects_its_contents():
    out = convert("use `a **b** c` here")
    assert "<code>a **b** c</code>" in out


def test_bold_spanning_a_code_span():
    # The bug this pins: splitting on code spans left `**` in separate fragments, so neither matched
    # and 72 literal asterisk pairs shipped across the docs.
    out = convert("**the `ENGINE=x` build**")
    assert "<strong>" in out and "**" not in out


def test_lists_and_blockquotes():
    assert convert("- one\n- two\n").count("<li>") == 2
    assert convert("1. one\n2. two\n").startswith("<ol>")
    assert "<blockquote>" in convert("> quoted\n")


def test_a_lone_image_is_a_figure():
    out = convert("![diagram](../media/x.svg)")
    assert "<figure>" in out and 'alt="diagram"' in out


def test_links_are_rewritten_by_the_caller():
    out = convert("[x](../dev/y.md)", lambda h: f"https://example.com/{h}")
    assert 'href="https://example.com/../dev/y.md"' in out


def test_the_rewriter_cannot_smuggle_a_bad_scheme_past_the_sanitiser():
    # Sanitising happens AFTER rewriting, so a rewriter that returns something dangerous is still
    # caught. It is the final URL that reaches the page, so the final URL is what is checked.
    assert 'href="#"' in convert("[x](ok.md)", lambda h: "javascript:alert(1)")


# --- safety --------------------------------------------------------------------------------------


def test_source_markup_is_escaped_not_executed():
    out = convert("a <script>alert(1)</script> b\n\n> <iframe src=x></iframe>\n")
    assert "<script" not in out.lower()
    assert "<iframe" not in out.lower()
    assert "&lt;script&gt;" in out


def test_event_handler_attributes_cannot_appear_in_a_tag():
    out = convert('<img src=x onerror=alert(1)>')
    assert not re.search(r"<[a-z][^>]*\son\w+\s*=", out, re.I)


def test_dangerous_url_schemes_are_dropped():
    for bad in ("javascript:alert(1)", "data:text/html,x", "vbscript:msgbox"):
        out = convert(f"[x]({bad})")
        assert bad.split(":")[0] not in out, f"{bad} survived into an href"


def test_ordinary_urls_survive():
    for good in ("https://example.com/a", "../dev/x.md", "#anchor", "media/x.svg", "mailto:a@b.c"):
        assert good in convert(f"[x]({good})"), f"{good} was wrongly dropped"


# --- the real documents --------------------------------------------------------------------------


def _docs():
    return [p for p in sorted(DOCS.glob("*.md")) if p.stem != "README"]


def test_every_engine_doc_converts_without_leftovers():
    for path in _docs():
        out = convert(path.read_text(encoding="utf-8"), skip_h1=True)
        prose = re.sub(r"<pre>.*?</pre>|<code>.*?</code>", "", out, flags=re.S)
        assert "**" not in prose, f"{path.name}: literal ** survived"
        assert not re.search(r"^\s*\|", prose, re.M), f"{path.name}: unconverted table row"
        assert not re.search(r"\[[^\]]+\]\([^)]+\)", prose), f"{path.name}: unconverted link"


def test_every_engine_doc_produces_balanced_tags():
    from html.parser import HTMLParser

    class Balance(HTMLParser):
        def __init__(self):
            super().__init__()
            self.stack = []
            self.bad = []

        def handle_starttag(self, tag, attrs):
            if tag not in ("img", "hr", "br"):
                self.stack.append(tag)

        def handle_endtag(self, tag):
            if not self.stack or self.stack[-1] != tag:
                self.bad.append(tag)
            else:
                self.stack.pop()

    for path in _docs():
        b = Balance()
        b.feed(convert(path.read_text(encoding="utf-8"), skip_h1=True))
        assert not b.bad and not b.stack, f"{path.name}: unbalanced {b.bad or b.stack}"
