# Web Installer

<https://marklynch.github.io/pool-controller-code/> flashes the latest release onto an ESP32-C6 straight from the browser using [ESP Web Tools](https://github.com/esphome/esp-web-tools) and Web Serial — no ESP-IDF toolchain, no esptool. It needs desktop Chrome or Edge; Safari, Firefox and mobile browsers have no Web Serial support and get a "flash manually" fallback instead.

The page offers the **5 most recent releases** in a version picker, defaulting to the latest. Picking an older one shows a rollback warning. ESP Web Tools reads the manifest at click time, so switching versions just re-points the install button at that version's manifest.

## Page source

The page source lives in `installer/`:

- `installer/index.html` — the installer page. Static; it reads `versions.json` at runtime to build the picker, so it needs no build step.
- `installer/app.js` — the page's script. Kept out of the HTML so the page's CSP can be `script-src 'self'` with no `'unsafe-inline'`.
- `installer/manifest.template.json` — the ESP Web Tools manifest, with `__VERSION__` / `__BINARY__` placeholders stamped once per offered version.
- `installer/build-site.sh` — assembles the deployable site.
- `installer/preview.sh` — builds it and serves it locally.

The published site is *not* these files as-is. `build-site.sh` queries the recent releases, downloads each `pool-controller-full-<tag>.bin`, and emits per-version manifests plus a `versions.json` index — none of which exist in the repo. Both the workflow and `preview.sh` call that one script, so a local preview is exactly what gets deployed. Serving the binaries same-origin rather than linking them from the releases keeps the flasher's fetch simple.

Releases without a `pool-controller-full-*.bin` asset are skipped, and the script keeps looking further back until it has 5 usable ones. Change the count via the `INSTALLER_VERSIONS` env in `pages.yml`; at ~1.5 MB per binary, 5 versions is roughly a 7.5 MB site.

A deploy always publishes the newest 5 releases regardless of which tag triggered it, so re-running an old tag's build can't roll the public installer back to that version.

## Vendored flasher

ESP Web Tools is **vendored into the site**, not loaded from a CDN. The page holds a live Web Serial port and writes a full flash image, so the JavaScript it loads decides which bytes reach the device — that code is pinned and verified rather than resolved at page load from a third-party origin. A floating `@10` CDN range would mean every visitor executing whatever was published to npm most recently, with no review and no deploy step in between.

`build-site.sh` downloads the exact `EWT_VERSION` tarball from the npm registry, checks it against the registry's published `dist.integrity` hash pinned in `EWT_INTEGRITY`, and unpacks it to `ewt/`. A mismatch aborts the build. To upgrade, bump both:

```bash
curl -s https://registry.npmjs.org/esp-web-tools/<version> | jq -r .dist.integrity
```

then re-test flashing on real hardware before merging.

With every asset same-origin, the page sets a `default-src 'none'` CSP allowing `script-src 'self'` and no external origins, so nothing off-origin can inject code into the flashing page.

## GitHub Pages setup

**One-time setup:** in Settings → Pages, set the source to **GitHub Actions**. Setting it to "Deploy from a branch" cannot work — that serves the sources raw, with no `manifest.json` and no binary, and the page fails with *"Failed to download manifest"*.

This is also why the installer sources live in `installer/` and not `docs/`: with a branch source, GitHub offers `/docs` as a publishing directory, and having the installer there invites exactly the misconfiguration above. The repo's `docs/` directory (this file included) holds Markdown only and is never published — that stays true only while the Pages source is GitHub Actions, so keep it there.

## Deploy triggers

- **Tag builds** — `workflow_run` on Build & Release, filtered to tags. Not `release: published`: releases published with the built-in `GITHUB_TOKEN` (as `build.yml` does) don't trigger further workflow runs, so that trigger would silently never fire.
- **Pushes to `main` touching `installer/`** — republishes the page against the current latest release.
- **Manual** — Actions tab → Deploy Web Installer → Run workflow, also against the latest release.

Runs of Build & Release that aren't tag builds show up as *skipped* here. That's expected: there's no release for them to publish.

## Testing locally

```bash
./installer/preview.sh                                   # latest 5 releases
./installer/preview.sh --limit 2                         # fewer, quicker to download
./installer/preview.sh --bin build/pool-controller-full.bin --version dev
```

This serves the assembled site on <http://localhost:8000>, which counts as a secure context, so the flasher really works. Requires `gh` unless you pass `--bin`. Opening `installer/index.html` from disk does **not** work — `manifest.json` and `versions.json` are generated, and `file://` can't fetch them anyway.
