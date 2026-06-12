# Fluxgrid — Session Handoff

**Date:** 2026-06-12  
**Context:** Arduino library publishing preparation

---

## What Was Done This Session

The original codebase is a monorepo. The Arduino library lived at
`arduino/Fluxgrid/` and the Next.js web app at the repo root. Arduino Library
Manager requires `library.properties` to be at the **repository root**, so the
repo was split.

The user had already created two new GitHub repos:
- `lonely-binary/Fluxgrid` — Arduino library only (was empty)
- `lonely-binary/Fluxgrid-Web` — web/backend code (was empty)

All files from `arduino/Fluxgrid/` were pushed to the **root** of
`lonely-binary/Fluxgrid` (`main` branch, commit `e223a63`):

```
library.properties   ← version=0.9.1
README.md
keywords.txt
src/Fluxgrid.h
src/Fluxgrid.cpp
examples/Quickstart/
examples/SensorsAndRelay/
examples/EventButton/
examples/GenericNode/
examples/LedControl/
examples/WiFiScan/
examples/SerialMonitor/
```

---

## What Still Needs to Be Done

### 1. Migrate web code → `lonely-binary/Fluxgrid-Web`

This session had no GitHub access to that repo. The following files/dirs from
this monorepo need to be pushed there:

- `src/` — Next.js source
- `public/` — static assets
- `migrations/` — database migrations
- `docs/` — documentation
- `scripts/` — build scripts
- `Dockerfile`
- `next.config.ts`
- `package.json`, `package-lock.json`, `bun.lock`
- `tsconfig.json`
- `eslint.config.mjs`
- `postcss.config.mjs`
- `vitest.config.ts`
- `.github/workflows/ci.yml`
- `.github/workflows/deploy-vps2.yml`
- `CLAUDE.md` (update any path references from `arduino/Fluxgrid/` → root)

> ⚠️ `deploy-vps2.yml` uses a self-hosted runner tagged `[self-hosted, vps2]`.
> After migrating, confirm the runner is registered to the new repo.

### 2. Fix `/docs/library` route in the web app

`CLAUDE.md` states the `/docs/library` page renders `arduino/Fluxgrid/README.md`.
After the split, that file no longer exists in the web repo. The route needs to
be updated to fetch `README.md` from `lonely-binary/Fluxgrid` instead — either
via the GitHub raw URL or the GitHub API.

### 3. Submit to Arduino Library Manager (one-time)

Open a PR against https://github.com/arduino/library-registry and add one line
to `repositories.txt`:

```
https://github.com/Lonely-Binary/Fluxgrid
```

Once merged, the library appears in Arduino IDE's Library Manager under the
name "Fluxgrid".

---

## Ongoing Rules (from CLAUDE.md)

Every time anything under the Arduino library changes:

1. **Bump the version** in `library.properties` (patch for fixes, minor for new
   API, major for breaking changes). Never ship without bumping.
2. **Update `README.md`** in the same commit so the public `/docs/library` page
   stays in sync.

Future release flow:
1. Make changes, bump `version=` in `library.properties`, update `README.md`
2. Push to `lonely-binary/Fluxgrid`
3. Tag the commit (e.g. `v0.9.2`) — Arduino Library Manager picks it up automatically

---

## Key Files

| File | Purpose |
|---|---|
| `library.properties` | Version, dependencies — must bump on every library change |
| `src/Fluxgrid.h` | Public API + inline `begin()` |
| `src/Fluxgrid.cpp` | Implementation: WiFi, MQTT, LED, autoConfig, OTA |
| `README.md` | User-facing docs; also rendered at `/docs/library` |
