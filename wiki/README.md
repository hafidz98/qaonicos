# QaonicOS Wiki

A comprehensive, human- and AI-readable wiki for QaonicOS,
built with [MkDocs](https://www.mkdocs.org/) +
[Material for MkDocs](https://squidfunk.github.io/mkdocs-material/).
It lives **inside the repo** (`wiki/`) and is the readable layer
over the deep documents in `docs/` — every page synthesizes its
sources and cites them.

## Layout

| Path | What |
|---|---|
| `wiki/mkdocs.yml` | Site configuration (theme, nav, extensions) |
| `wiki/docs/*.md` | The wiki pages — **this is the content that gets committed** |
| `wiki/site/` | Build output — generated, gitignored, never committed |

## Build / serve locally

Prerequisites: Python 3.

```sh
pip install mkdocs mkdocs-material
# (or reuse a venv: python3 -m venv .venv && .venv/bin/pip install mkdocs mkdocs-material)

cd wiki
mkdocs serve            # live preview at http://127.0.0.1:8000
mkdocs build --strict   # CI-grade build into wiki/site/ (must pass clean)
```

## Conventions

- English throughout; one topic per page; every page opens with a
  short summary and a **Key facts** block before the prose.
- Facts trace to the deep docs in `../docs/`. Source citations are
  repo-relative links (e.g. `../docs/ARSITEKTUR-SISTEM.md`): they
  resolve when browsing the repository on GitHub. Inside the built
  site they intentionally point out of the wiki, which is why
  `mkdocs.yml` sets `validation.links.not_found: ignore` — do not
  "fix" those links into the wiki; add a wiki page instead.
- Where sources conflict, the newest research wins and the
  correction is stated on the page (see the corrections register in
  `../docs/riset-soc-rv1103/RINGKASAN.md`).
- Update the **Last updated** stamp on `docs/ai-index.md` whenever
  pages change materially.
