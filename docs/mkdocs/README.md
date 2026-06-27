# QMX Panadapter Documentation (MkDocs)

This is the source for the official documentation site at **qmxpanadapter.lav.dk**.

## Quick Start (Local Development)

### Install dependencies

```bash
pip install -r ../requirements.txt
```

Or install individually:

```bash
pip install mkdocs mkdocs-material mike
```

### Run the dev server

```bash
mkdocs serve
```

Open `http://localhost:8000` in your browser. Changes to Markdown files reload automatically.

### Build static site

```bash
mkdocs build
```

Output is in `site/` — ready to upload to a web server.

## File Structure

```
docs/mkdocs/
  index.md                  Home page
  quick-start.md            10-minute getting started
  guide/
    panadapter.md           Spectrum, waterfall, tap-to-tune
    ft8-rx.md               FT8 decoding, decode list
    ft8-tx.md               FT8 transmit, QSO exchange, robot mode
    web-ui.md               Remote control, browser interface
    time-sync.md            SNTP, RTC, manual time, offline operation
    settings.md             Configuration & preferences
  reference/
    gestures.md             Touch controls, swipes, taps
    hardware.md             Tab5 specs, QMX CAT reference, antennas
    web-api.md              REST API endpoints, JSON examples
    troubleshooting.md      Common issues & solutions
  build/
    build.md                ESP-IDF setup & build steps
    architecture.md         Module map, data flow, task priorities
    contributing.md         Dev workflow, PR guidelines, areas of focus
  releases.md               Version history, changelog, roadmap

mkdocs.yml                  MkDocs configuration (site nav, theme, etc.)
```

## Editing Content

Each `.md` file is a page. The `mkdocs.yml` nav section controls:

- Page order
- Menu hierarchy
- Page titles

**Example:** to add a new page:

1. Create `docs/mkdocs/guide/new-feature.md`
2. Add to `mkdocs.yml` nav:
   ```yaml
   nav:
     - Guide:
       - New Feature: guide/new-feature.md
   ```
3. Run `mkdocs serve` — the page appears automatically

## Formatting

Uses **CommonMark** Markdown + Material extensions:

```markdown
# Heading 1
## Heading 2

**bold** *italic* `code`

> blockquote

[Link text](../relative/path.md)

| Col 1 | Col 2 |
|-------|-------|
| A     | B     |

=== "Tab 1"
    Content here

=== "Tab 2"
    Other content

!!! note
    Admonition box
```

See [Material Markdown](https://squidfunk.github.io/mkdocs-material/reference/) for full syntax.

## Deployment

See [DEPLOY_DOCS.md](../../DEPLOY_DOCS.md) in the repo root for:

- Manual upload (SCP)
- Versioned releases (mike)
- GitHub Actions auto-deploy
- Web server setup (Nginx/Apache)
- SSL certificate (Let's Encrypt)

## Keeping Docs in Sync

The docs are part of the Git repo. After each release:

1. Update version numbers in relevant pages
2. Add release notes to `releases.md`
3. Commit: `git commit -am "docs: release v0.18.8"`
4. Push to main
5. GitHub Actions auto-deploys (if configured)

## Theme & Styling

Using **Material for MkDocs** — includes:

- Automatic table of contents
- Search (client-side)
- Dark/light mode toggle
- Mobile responsive
- Code syntax highlighting

Customization is in `mkdocs.yml` (colors, fonts, features).

## Tips

- Keep pages **focused** — one topic per page
- Use **relative links** — `[link](../other.md)` works everywhere
- Link **backward** — reference earlier sections readers may need context from
- Keep **headings short** — they appear in the ToC
- Test **locally** before pushing — typos and dead links are obvious in the dev server

## Questions?

- See [Material for MkDocs docs](https://squidfunk.github.io/mkdocs-material/)
- Check existing pages for examples
- Read [DEPLOY_DOCS.md](../../DEPLOY_DOCS.md) for deployment issues

---

**Ready to edit?** Pick a page, run `mkdocs serve`, and start writing! 🚀
