#!/usr/bin/env python3
"""Checks a built site (scripts/build_site.sh) for broken internal links:
every relative link and image of every page must lead to a file of the
site, and a link to a #section to a heading or other element with that id.
Links to other sites aren't followed. Exits 1, listing them, if any are
broken."""

import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.links = []  # (attribute, url)

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if value is None:
                continue
            if name in ("id", "name") and (name == "id" or tag == "a"):
                self.ids.add(value)
            elif (name == "href" and tag in ("a", "link")) or (name == "src" and tag in ("img", "script")):
                self.links.append(value)


def parse(path: Path) -> Page:
    p = Page()
    p.feed(path.read_text(encoding="utf-8"))
    return p


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} SITE_DIR")
    root = Path(sys.argv[1]).resolve()
    pages = {}

    def page(path):
        if path not in pages:
            pages[path] = parse(path)
        return pages[path]

    broken = []
    for html_file in sorted(root.rglob("*.html")):
        for url in page(html_file).links:
            parts = urlsplit(url)
            if parts.scheme or parts.netloc:
                continue  # another site, or an absolute address
            if parts.path:
                target = (html_file.parent / unquote(parts.path)).resolve()
                if url.endswith("/") or target.is_dir():
                    target = target / "index.html"
            else:
                target = html_file
            where = html_file.relative_to(root)
            if not target.is_relative_to(root) or not target.exists():
                broken.append(f"{where}: {url} (no such file)")
                continue
            if parts.fragment and target.suffix == ".html":
                if unquote(parts.fragment) not in page(target).ids:
                    broken.append(f"{where}: {url} (no #{parts.fragment} there)")
    for line in broken:
        print(line)
    print(f"{len(pages)} pages checked, {len(broken)} broken link(s)")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
