#!/usr/bin/env python3
"""Builds the project site: site/ as it is, plus the user manual and the
camera format documents rendered from their Markdown, one page each, and a
sitemap of every page. Run by scripts/build_site.sh, which picks the
manual's version; see there.

Each section (manual, formats) is a folder of Markdown files with a README
whose table lists the pages in order: the README becomes /<section>/, every
other page /<section>/<name>/. The page list beside each page follows that
table; pages it doesn't list come after it, by name.
"""

import argparse
import html
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SITE_URL = "https://schabo.github.io/orbis-reframe/"
REPO = "https://github.com/Schabo/orbis-reframe"
HERE = Path(__file__).resolve().parent


def table_pages(readme: Path):
    """(file name, label) for each row of the README's table that links a
    page: the label is the row's first cell, as plain text."""
    out = []
    for line in readme.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        link = re.search(r"\]\(([^)#/]+\.md)\)", line)
        if not link:
            continue
        first = line.strip("|").split("|")[0]
        label = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", first)
        label = label.replace("`", "").strip()
        out.append((link.group(1), label))
    return out


def page_order(src: Path):
    listed = [(f, label) for f, label in table_pages(src / "README.md") if (src / f).exists()]
    names = {f for f, _ in listed}
    rest = sorted(p.name for p in src.glob("*.md") if p.name != "README.md" and p.name not in names)
    for f in rest:
        heading = next((l[2:].strip() for l in (src / f).read_text(encoding="utf-8").splitlines()
                        if l.startswith("# ")), f[:-3])
        listed.append((f, heading))
    return listed


def build_section(section, src, ref, version_note, out, nav_label, contents_label, sitemap):
    pages = page_order(src)
    entries = [("README.md", contents_label)] + pages
    for i, (name, _) in enumerate(entries):
        stem = name[:-3]
        is_readme = name == "README.md"
        root = "../" if is_readme else "../../"
        section_root = "" if is_readme else "../"  # from the page to /<section>/

        def href(other):
            return section_root + ("" if other == "README.md" else other[:-3] + "/")

        nav = ["<ol>"]
        for other, label in entries:
            current = ' aria-current="page"' if other == name else ""
            nav.append(f'<li><a href="{href(other)}"{current}>{html.escape(label)}</a></li>')
        nav.append("</ol>")

        steps = []
        if i > 0:
            prev, label = entries[i - 1]
            steps.append(f'<a class="prev" href="{href(prev)}"><span>Previous</span>'
                         f"{html.escape(label)}</a>")
        if i + 1 < len(entries):
            nxt, label = entries[i + 1]
            steps.append(f'<a class="next" href="{href(nxt)}"><span>Next</span>'
                         f"{html.escape(label)}</a>")

        path = f"{section}/" + ("" if is_readme else stem + "/")
        target = out / path / "index.html"
        target.parent.mkdir(parents=True, exist_ok=True)
        cmd = [
            "pandoc", str(src / name),
            "--from", "gfm", "--to", "html5", "--wrap", "preserve",
            "--template", str(HERE / "page.html"),
            "--lua-filter", str(HERE / "site.lua"),
            "--metadata", f"site_section={section}",
            "--metadata", f"site_page={stem}",
            "--metadata", f"site_ref={ref}",
            "--metadata", f"site_repo={REPO}",
            "--variable", f"root={root}",
            "--variable", f"in_{section}=true",
            "--variable", f"nav={''.join(nav)}",
            "--variable", f"nav_label={nav_label}",
            "--variable", f"canonical={SITE_URL}{path}",
            "--variable", f"version_note={version_note(name)}",
            "--variable", f"source={REPO}/blob/{ref}/docs/{section}/{name}",
            "--output", str(target),
        ]
        if steps:
            cmd += ["--variable", f"steps={''.join(steps)}"]
        subprocess.run(cmd, check=True)
        sitemap.append(SITE_URL + path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--site", type=Path, required=True, help="the project page's folder (site/)")
    ap.add_argument("--manual", type=Path, required=True, help="docs/manual/ of the version to publish")
    ap.add_argument("--manual-ref", required=True, help="its tag, or main")
    ap.add_argument("--formats", type=Path, required=True, help="docs/formats/")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    shutil.copytree(args.site, args.out)
    sitemap = [SITE_URL]

    release = args.manual_ref.startswith("v")

    def manual_note(name):
        main_url = f"{REPO}/blob/main/docs/manual/{name}"
        if release:
            return (f"The manual of orbis-reframe {html.escape(args.manual_ref[1:])}, the "
                    f'newest release. What has changed since is in the '
                    f'<a href="{main_url}">development version on GitHub</a>.')
        return (f'The manual of the development version, not yet released. '
                f'<a href="{main_url}">On GitHub</a>.')

    def formats_note(name):
        return ("What orbis-reframe knows about the cameras' recorded files, kept up to date "
                f'with its development. <a href="{REPO}/blob/main/docs/formats/{name}">'
                "On GitHub</a>.")

    build_section("manual", args.manual, args.manual_ref, manual_note, args.out,
                  "User manual", "Contents", sitemap)
    images = args.manual / "images"
    if images.is_dir():
        shutil.copytree(images, args.out / "manual" / "images")
    build_section("formats", args.formats, "main", formats_note, args.out,
                  "Camera file formats", "All cameras", sitemap)

    with open(args.out / "sitemap.xml", "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write('<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n')
        for url in sitemap:
            f.write(f"  <url>\n    <loc>{html.escape(url)}</loc>\n  </url>\n")
        f.write("</urlset>\n")
    print(f"{len(sitemap)} pages in {args.out}")


if __name__ == "__main__":
    sys.exit(main())
