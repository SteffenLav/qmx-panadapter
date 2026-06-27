# Deploying the MkDocs Site

The QMX Panadapter documentation is built with **MkDocs** and deployed to **tab5.lav.dk**.

## Prerequisites

- **MkDocs** — `pip install mkdocs mkdocs-material`
- **mike** — `pip install mike` (for versioning)
- **Hosting** — web server with access to your domain

## Local Development

### Build Locally

```bash
mkdocs serve
```

This runs a development server on `http://localhost:8000` with hot-reload. Edit Markdown files in `docs/mkdocs/` and changes appear instantly in the browser.

### Build Static Site

```bash
mkdocs build
```

This generates the static site in the `site/` directory. You can copy this folder to any web server.

## Deployment (Manual)

### Option 1: Copy to Web Server (Simplest)

```bash
mkdocs build
scp -r site/* user@your-server:/var/www/tab5.lav.dk/
```

Replace:
- `user` — your SSH username
- `your-server` — your web server hostname or IP
- `/var/www/tab5.lav.dk/` — path on your server (adjust to your setup)

### Option 2: Using mike (Versioned)

**mike** automatically maintains a `/latest` symlink and a version selector dropdown:

```bash
# Build and deploy for v0.18.8
mike deploy 0.18.8 latest

# Deploy and set the default version
mike deploy 0.18.8 latest --update-aliases
```

This creates a site structure like:

```
tab5.lav.dk/
  latest/          → symlink to current release
  0.18.8/
  0.18.7/
  0.18.6/
  ...
```

Readers can switch versions using the dropdown in the top-right corner.

**Setup mike hosting:**

1. Deploy via SCP or Git (see below)
2. Ensure your web server is configured to serve the site root (default: `index.html`)

## Deployment (GitHub Actions)

Set up automatic deployment on each release:

### 1. Create `.github/workflows/docs.yml`

```yaml
name: Deploy Docs

on:
  push:
    branches:
      - main
    paths:
      - 'docs/mkdocs/**'
      - 'mkdocs.yml'
  workflow_dispatch:

jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Set up Python
        uses: actions/setup-python@v4
        with:
          python-version: '3.10'
      
      - name: Install dependencies
        run: |
          pip install mkdocs mkdocs-material mike
      
      - name: Build and deploy
        env:
          DEPLOY_HOST: ${{ secrets.DEPLOY_HOST }}
          DEPLOY_USER: ${{ secrets.DEPLOY_USER }}
          DEPLOY_KEY: ${{ secrets.DEPLOY_KEY }}
          DEPLOY_PATH: ${{ secrets.DEPLOY_PATH }}
        run: |
          # Generate SSH key from secret
          mkdir -p ~/.ssh
          echo "$DEPLOY_KEY" > ~/.ssh/deploy_key
          chmod 600 ~/.ssh/deploy_key
          ssh-keyscan -H $DEPLOY_HOST >> ~/.ssh/known_hosts
          
          # Build site
          mkdocs build
          
          # Deploy
          scp -i ~/.ssh/deploy_key -r site/* ${DEPLOY_USER}@${DEPLOY_HOST}:${DEPLOY_PATH}/
```

### 2. Add GitHub Secrets

In your GitHub repository settings (**Settings → Secrets and variables → Actions**), add:

- **DEPLOY_HOST** — your server's hostname (e.g., `lav.dk`)
- **DEPLOY_USER** — SSH username (e.g., `webmaster`)
- **DEPLOY_KEY** — private SSH key (for passwordless deploy)
- **DEPLOY_PATH** — server path (e.g., `/var/www/tab5.lav.dk`)

### 3. Generate SSH Key (One-Time Setup)

On your server:

```bash
ssh-keygen -t ed25519 -f /home/webmaster/.ssh/github_deploy
cat ~/.ssh/github_deploy.pub >> ~/.ssh/authorized_keys
chmod 700 ~/.ssh
chmod 600 ~/.ssh/authorized_keys
```

Then copy the **private key** (`github_deploy`) to GitHub Secrets as `DEPLOY_KEY`.

## Web Server Configuration

### Nginx

```nginx
server {
    server_name tab5.lav.dk;
    
    root /var/www/tab5.lav.dk;
    index index.html;
    
    location / {
        try_files $uri $uri/ /index.html;
    }
    
    # Cache static assets for 1 year
    location ~* \.(css|js|png|jpg|svg|woff|woff2)$ {
        expires 1y;
        add_header Cache-Control "public, immutable";
    }
}
```

### Apache

```apache
<VirtualHost *:80>
    ServerName tab5.lav.dk
    DocumentRoot /var/www/tab5.lav.dk
    
    <Directory /var/www/tab5.lav.dk>
        Options -MultiViews
        RewriteEngine On
        RewriteCond %{REQUEST_FILENAME} !-f
        RewriteRule ^ index.html [QSA,L]
    </Directory>
</VirtualHost>
```

## SSL Certificate

Use **Let's Encrypt** (free):

```bash
sudo certbot certonly --webroot -w /var/www/tab5.lav.dk -d tab5.lav.dk
```

Then update your Nginx/Apache config to use the certificate.

## Updating the Site

Every time you:

1. Update Markdown files in `docs/mkdocs/`
2. Update `mkdocs.yml` config
3. Commit and push to `main`

GitHub Actions automatically rebuilds and deploys the site. You'll see a green checkmark in your commit history when the deploy succeeds.

## Manual Deploy (Testing)

To test deployment locally before automating:

```bash
# Build
mkdocs build

# Test locally
mkdocs serve

# Upload to server (adjust SSH details)
scp -r site/* user@tab5.lav.dk:/var/www/
```

## Troubleshooting

**Site doesn't appear after deploy?**

1. Check files were uploaded: `ls -la /var/www/tab5.lav.dk/`
2. Check permissions: files should be readable by the web server user
3. Check web server is running: `sudo systemctl restart nginx`
4. Check firewall: port 80 and 443 should be open

**Old version still showing?**

1. Clear your browser cache (Ctrl+Shift+Delete)
2. Force HTTPS reload (browsers cache aggressively)
3. Check web server cache headers (may need to clear)

**GitHub Actions failing?**

1. Check the workflow run in **Actions** tab
2. Look at the logs for SSH/auth errors
3. Verify secrets are set correctly (no typos)
4. Test SSH manually: `ssh -i <key> user@host`

---

## Next Steps

- Set up the domain (`tab5.lav.dk`) DNS to point to your server
- Test the site locally (`mkdocs serve`)
- Deploy to your server
- Enable HTTPS (Let's Encrypt)
- Set up GitHub Actions (optional, but recommended)

Enjoy your documentation site! 🚀
