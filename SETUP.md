# Ruju setup

## Render (Flask API)
- Root Directory: `website/backend`
- Build: `pip install -r requirements.txt`
- Start: `gunicorn wsgi:app --bind 0.0.0.0:$PORT --workers 1 --threads 4 --timeout 120`
- Env:
  - `ADMIN_PASSWORD`, `SECRET_KEY` (required)
  - `DATABASE_URL` (Neon, recommended)
  - `GITHUB_TOKEN` (repo write; contents under `website/docs/...` only)
  - `GITHUB_REPO=buxinelectronics-art/Ruju`
  - `GITHUB_BRANCH=main`
  - `DEFAULT_CONTENT_URL=https://buxinelectronics-art.github.io/Ruju`
  - optional `FRONTEND_ORIGIN=https://buxinelectronics-art.github.io`

## GitHub Pages
- Workflow deploys `website/docs` on push to `main`.
- Admin UI: `https://buxinelectronics-art.github.io/Ruju/`

## Arduino
- Open `QuranPlayerV7/QuranPlayerV7.ino` (needs sibling `huruf_bitmaps.h`)
- Board: ESP32-C3, Huge APP, USB CDC On Boot Enabled
- Libraries: U8g2, arduino-libhelix
- Set `PRODUCT_ID` / `DEVICE_TOKEN` / `BACKEND_HOST` from the dashboard

## Regenerate assets
```bash
python website/tools/make_huruf_bitmaps.py
python website/tools/make_excel_templates.py
```
