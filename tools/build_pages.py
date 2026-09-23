#!/usr/bin/env python3
"""Build the GitHub Pages copy of the setup page: the page the board serves
(src/web_page.h), with web/relay.js ahead of its script so every API call goes
through the remote-setup relay. Writes site/index.html."""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
header = (root / "src/web_page.h").read_text()
m = re.search(r'R"HTML\((.*)\)HTML"', header, re.S)
if not m:
    sys.exit("build_pages: no R\"HTML(...)HTML\" literal in src/web_page.h")
page = m.group(1)
relay = (root / "web/relay.js").read_text()

# Scripts and styles are inline; the only thing the page may connect to is a
# relay server over wss.
csp = ('<meta http-equiv="Content-Security-Policy" content="default-src \'none\'; '
       'script-src \'unsafe-inline\'; style-src \'unsafe-inline\'; connect-src wss:; img-src data:">\n'
       '<meta name="referrer" content="no-referrer">\n')
page = page.replace('<meta charset="utf-8">\n', '<meta charset="utf-8">\n' + csp, 1)
page = page.replace("<title>claude-status</title>", "<title>claude-status remote setup</title>", 1)
if "<script>\n" not in page:
    sys.exit("build_pages: no <script> in the page")
page = page.replace("<script>\n", "<script>\n" + relay + "</script>\n<script>\n", 1)

out = root / "site"
out.mkdir(exist_ok=True)
(out / "index.html").write_text(page)
(out / ".nojekyll").write_text("")
print(f"build_pages: wrote {out / 'index.html'} ({len(page)} bytes)")
