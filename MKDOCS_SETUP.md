# MkDocs Setup Checklist

Your QMX Panadapter documentation site is ready to go! Here's how to get it live.

## ✅ What's Already Done

- **mkdocs.yml** — site configuration with full navigation
- **docs/mkdocs/** — complete documentation structure:
  - `index.md` — home page with features & quick links
  - `quick-start.md` — 10-minute getting-started guide
  - **guide/** — user manual (panadapter, FT8, web UI, time sync, settings)
  - **reference/** — technical reference (gestures, hardware, API, troubleshooting)
  - **build/** — developer docs (build, architecture, contributing)
  - `releases.md` — version history & roadmap
- **.github/workflows/deploy-docs.yml** — GitHub Actions workflow for auto-deploy
- **DEPLOY_DOCS.md** — full deployment guide (manual or automated)
- **docs/requirements.txt** — Python dependencies (pip install)

## 🚀 To Get Started Locally

```bash
# Install dependencies
pip install -r docs/requirements.txt

# Run dev server
mkdocs serve

# Open browser to http://localhost:8000
```

Changes to files in `docs/mkdocs/` appear instantly.

## 🌐 To Deploy to tab5.lav.dk

### Option 1: Manual Deploy (Quickest for Testing)

```bash
# Build static site
mkdocs build

# Upload to your server (adjust path/user/host)
scp -r site/* user@lav.dk:/var/www/tab5.lav.dk/
```

### Option 2: GitHub Actions (Fully Automated)

1. **Add GitHub Secrets** (Settings → Secrets and variables → Actions):
   - `DEPLOY_HOST` — your server hostname (e.g., `lav.dk`)
   - `DEPLOY_USER` — SSH username (e.g., `www-data`)
   - `DEPLOY_KEY` — private SSH key (for passwordless deploy)
   - `DEPLOY_PATH` — server path (e.g., `/var/www/tab5.lav.dk`)

2. **Every time you push to main**, GitHub Actions automatically:
   - Builds the site
   - Deploys to your server
   - Shows status in the commit history

See **DEPLOY_DOCS.md** for detailed setup (SSH key generation, web server config, etc.).

## 📝 To Update the Documentation

Just edit files in `docs/mkdocs/`:

1. **User Guide** — edit `guide/*.md` files
2. **Troubleshooting** — edit `reference/troubleshooting.md`
3. **Release Notes** — edit `releases.md`
4. **Add a new page:**
   - Create `docs/mkdocs/topic/page.md`
   - Add to nav in `mkdocs.yml`
5. **Commit & push** — auto-deploys if GitHub Actions is configured

## 🔗 Site Navigation

The nav structure (from `mkdocs.yml`):

```
Home
├─ Quick Start
├─ User Guide
│  ├─ Panadapter
│  ├─ Web UI
│  ├─ FT8 RX
│  ├─ FT8 TX
│  ├─ Time Sync
│  └─ Settings
├─ Reference
│  ├─ Gestures & Controls
│  ├─ Hardware
│  ├─ Web API
│  └─ Troubleshooting
├─ For Builders
│  ├─ Build from Source
│  ├─ Architecture
│  └─ Contributing
└─ Releases
```

Edit `mkdocs.yml` to change the structure.

## 🎨 Customization

**To change colors/fonts/theme:**

Edit `mkdocs.yml`:

```yaml
theme:
  palette:
    - scheme: light
      primary: blue          # Change these
      accent: orange         # to your preferred colors
```

Material theme colors: red, pink, purple, deep-purple, indigo, blue, light-blue, cyan, teal, green, light-green, lime, yellow, amber, orange, deep-orange, brown, grey, blue-grey.

**To add your logo:**

```yaml
theme:
  logo: path/to/logo.png
```

## 📊 Analytics (Optional)

To track site visits, add Google Analytics:

```yaml
plugins:
  - search
  - analytics:
      provider: google
      property: G-XXXXXXXXXX
```

Get your Google Analytics ID from [Google Analytics](https://analytics.google.com).

## 🔒 SSL Certificate (Recommended)

Use Let's Encrypt (free):

```bash
sudo certbot certonly --webroot \
  -w /var/www/tab5.lav.dk \
  -d tab5.lav.dk
```

Then configure your web server (Nginx/Apache) to use the certificate. See **DEPLOY_DOCS.md** for examples.

## ✨ Next Steps

1. **Test locally:** `mkdocs serve` → http://localhost:8000
2. **Read DEPLOY_DOCS.md** for deployment options
3. **Set up domain DNS** to point to your server
4. **Deploy** (manual SCP or GitHub Actions)
5. **Enable HTTPS** with Let's Encrypt

## 📚 Resources

- [Material for MkDocs docs](https://squidfunk.github.io/mkdocs-material/)
- [Markdown syntax](https://squidfunk.github.io/mkdocs-material/reference/abbreviations/)
- [DEPLOY_DOCS.md](DEPLOY_DOCS.md) — full deployment guide
- [docs/mkdocs/README.md](docs/mkdocs/README.md) — editing guide

## 🆘 Troubleshooting

**mkdocs command not found?**

```bash
pip install mkdocs mkdocs-material
```

**Port 8000 already in use?**

```bash
mkdocs serve -a localhost:8001
```

**Dead links in the built site?**

Run a link checker:

```bash
pip install mkdocs-redirects
# then fix broken links in the .md files
```

**Content not updating?**

1. Clear your browser cache (Ctrl+Shift+Delete)
2. Restart `mkdocs serve`
3. Check file was saved (look at timestamps)

---

**You're all set!** Start with:

```bash
mkdocs serve
```

Then open http://localhost:8000 and explore. 🚀
