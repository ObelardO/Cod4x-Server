# FinalKillcamEntityCamera — version history

Plugin version is `PLUGIN_VER_MAJ.PLUGIN_VER_MIN` in `final_killcam_entity_camera.c`.

Requires CoD4x server with **snapshot patch API** (`plugin_snapshot_patching.c` on branch `snapshot-patch-api` or newer).

## Usage notes

### Entity killcam (heli, grenade, claymore, etc.)

```gsc
SetFinalKillcamTargetEntity( victim getEntityNumber() );
// set killcamentity for broadcast viewers as usual
```

Clear when killcam ends: `SetFinalKillcamTargetEntity( -1 );`

### Suicide final killcam

This plugin is **entity killcam only**. It activates when viewers have an active `killCamEntity` and `SetFinalKillcamTargetEntity` is set.

For **suicide** broadcast killcams, handle everything in **GSC** — do not use this plugin on that path:

- Do **not** call `SetFinalKillcamTargetEntity( victim )` (or call `SetFinalKillcamTargetEntity( -1 )` to clear).
- Do **not** set `killcamentity` for broadcast viewers (`killcamentity( -1 )` or omit it).
- Use stock archived / victim replay killcam instead.

With no `killCamEntity`, the plugin is a no-op for suicide killcams.

---

## 5.30

- Adapted for **chained snapshot hooks** (multiple plugins can patch the same snapshot).
- `FK_PatchEntity` / `FK_PatchClientState`: `switch` on `SNAPSHOT_PATCH_MODIFY` vs `SNAPSHOT_PATCH_APPEND`.
- MODIFY: spawn latch runs before session guard; hide/proxy only when `FK_SnapMatchesClient`.
- APPEND: inject proxy only for the active snapshot client.
- Init note: load this plugin **after** other snapshot patch plugins when possible.

## 5.29

- Snapshot callbacks use unified **MODIFY / APPEND** `mode` (3 core hooks instead of 5).

## 5.28

- `fkcam_log` cvar replaced with compile-time **`FKCAM_LOG`** define.

## 5.27

- **`fkcam_viewer_body`** removed; viewer body proxy always enabled.

## 5.26

- **`fkcam_debug`** removed; logging is console-only via `FKCAM_LOG`.

## 5.25

- **`fkcam_body_frozen`** removed; body always replays movement from archive.

## 5.24

- Code cleanup: split into **base logic** (origin spoof + hide) and **proxy model** blocks.
- Removed unused state (`s_snapShowProxy`, `s_killCamEntTracked`, etc.).

## 5.23

- Removed **`SetFinalKillcamEntitySpawnArchiveTime`** GSC API; spawn time auto-detected from snapshot.

## 5.22 and earlier

- Broadcast entity killcam aim fix for non-victim viewers.
- Origin spoof to victim archived position; hide viewer model.
- Body proxy on client slot `maxclients - 1`.
- Entity-phase POV gating (no patch during attacker follow).
- GSC: **`SetFinalKillcamTargetEntity(victimEntNum)`**.

---

## How to add a new entry

1. Bump `PLUGIN_VER_MIN` (or `PLUGIN_VER_MAJ` for breaking changes) in `final_killcam_entity_camera.c`.
2. Add a section above **“How to add a new entry”** using this template:

```markdown
## X.YY

- Short bullet of what changed.
- Another change if needed.
```

3. Rebuild: `make -C plugins/FinalKillcamEntityCamera`
4. Deploy `FinalKillcamEntityCamera.dll` with matching server build.
