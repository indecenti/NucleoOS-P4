# NucleoV2 App Store server

Reference HTTP server that the on-device store (`components/nv_appstore`) installs WASM apps from.
A proper little storefront: **categorized**, **multilingual**, and **region-aware**. Pure Python 3
standard library — no packages to install.

## Run

```sh
cd server/appstore
python appstore_server.py                 # serves the repo apps/ folder on 0.0.0.0:8090
```

Options:

| flag | default | meaning |
|------|---------|---------|
| `--apps-dir` | `../../apps` | folder of `<id>/{manifest.json,app.wasm}` apps to publish; repeat it to serve several (first one wins on an id clash) |
| `--host` | `0.0.0.0` | bind address (`127.0.0.1` = localhost only) |
| `--port` | `8090` | TCP port |

Open `http://<PC-ip>:8090/` in a browser to see the catalog.

## Point the device at it

On the tablet: **Settings → Update → App store** → enter `http://<PC-ip>:8090` → **SAVE STORE URL**.
Then open **Apps → Store**: the catalog loads over Wi-Fi and each app has **INSTALL** / **UPDATE**.
The module is written to `/sdcard/apps/<id>/` and its Home tile appears right away; installed apps
get an **OPEN** button. Long lists are paged (12 cards per page, in both tabs).

The device and the PC must be on the same LAN. HTTPS works too (the firmware attaches the ESP root-CA
bundle) — put this behind a reverse proxy with a real certificate to serve a public store.

## What it serves

```
GET /                            HTML index (human browsing; ?lang= switches language)
GET /store.json?lang=&region=    catalog, localized + region-filtered for the caller
GET /apps/<id>/manifest.json     one app's manifest.json
GET /apps/<id>/app.wasm          the module
GET /apps/<id>/icon.argb         optional 80×80 ARGB8888 launcher icon
GET /apps/<id>/app.aot           optional precompiled image (wamrc, see sdk/README.md)
```

An **app** is any sub-directory of an apps root containing both `manifest.json` and `app.wasm` —
the same layout the device uses under `/sdcard/apps/<id>/`; the folder name must be the app id.
`game` is derived (`abi ≥ 2` + the `gfx` permission, or a WASM-4 cart); `size` is the real
`app.wasm` byte count; `icon` reflects whether an `icon.argb` exists; `aot` is the `app.aot` byte
count (0 = none). The device installs `app.aot` together with the module and runs it natively (and
deletes a stale one when an update comes without). Descriptions are trimmed to 120 characters.

## Categories, languages, regions — `catalog.json`

Store metadata is curated centrally in [`catalog.json`](catalog.json) and merged over each manifest,
so the app folders stay clean. It has two sections:

- **`categories`** — ordered list of `{id, icon, name:{lang:…}}`. Category names are localized.
- **`apps`** — keyed by app id: `{category, featured, rating, downloads, regions:[…],
  names:{lang:…}, descriptions:{lang:…}}`. Any field is optional.

**Multilingual** — the device requests `?lang=it|en|es|fr|de` (its UI language). The server resolves
each app's name/description and every category name to that language (fallback: English → any). Add a
language by adding its key to the `name`/`names`/`descriptions` maps.

**Region-aware** — the device sends `?region=IT|US|EU|…` (Settings → Region). An app is shown only when
its `regions` list contains that region or `"*"`; `"EU"` matches any EU country. With no region the
server infers one from the language. Categories with no visible app are dropped. Apps are ordered
**featured → most-downloaded → name**.

Example `store.json?lang=it&region=IT` row:
```json
{ "id":"abc123", "name":"ABC 123", "description":"Impara lettere e numeri…",
  "category":"kids", "category_name":"Bambini", "featured":true, "rating":4.8,
  "abi":2, "size":60262, "game":true, "icon":true, "regions":["*"] }
```

## Publish an app

Building an app with `sdk/build_app.ps1` already produces `apps/<id>/{manifest.json,app.wasm}`, so if
you serve the repo `apps/` folder it is published automatically. To publish into a **separate** curated
store folder, copy it in:

```sh
python publish.py apps/pianino --store ./store-apps
python appstore_server.py --apps-dir ./store-apps
```

Optional `manifest.json` fields the store surfaces (ignored by the device runtime): `author`,
`description`. Without a `catalog.json` entry an app is listed with those and its manifest name, in
the `wasm4` category if it is a WASM-4 cart (`"wasm4": true`), otherwise in `other`.

## WASM-4 gallery

`tools/w4harness/store.sh` turns the wasm4.org gallery into a store folder: every cart that passed
the last `sweep.sh` in every input mode, with name, author and description from its gallery page and
an `app.aot` precompiled for the P4. Serve it next to the repo apps (the path is inside WSL, e.g.
`\\wsl.localhost\Ubuntu-24.04\root\w4harness\store-apps` from Windows):

```sh
python appstore_server.py --apps-dir ../../apps --apps-dir <path to store-apps>
```

The gallery carts are licensed **CC BY-NC-SA 4.0** by their authors: fine for your own device, keep
the author credit, no commercial use. Never commit them to this repo.

## Security notes

- App ids are validated against `^[A-Za-z0-9_-]{1,31}$` and only the four files above are served, so
  path traversal is refused (`../` → 404).
- The device also validates the WebAssembly / AOT magic and a size cap (2 MB module, 4 MB AOT image)
  before writing to the card, and
  WAMR sandboxes the guest with per-manifest permission gating. Still, **serve apps you trust** — set
  the store URL only to a host you control.
