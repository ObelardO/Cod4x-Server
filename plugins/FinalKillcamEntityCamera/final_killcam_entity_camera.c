#include "../pinc.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define PLUGIN_NAME "FinalKillcamEntityCamera"
#define PLUGIN_DESC "Fixes broadcast entity killcam aim using snapshot origin spoof"
#define PLUGIN_DESC_LONG "SetFinalKillcamTargetEntity(victimEntNum) during broadcast entity killcam so viewers aim at the victim."
#define PLUGIN_VER_MAJ 5
#define PLUGIN_VER_MIN 29

#define KILLCAM_ENTITY_NONE (MAX_GENTITIES - 1)
#define FK_PROXY_RANK_HIDDEN (-1)
#define DEBUG_INTERVAL_MS 1000
/* Log level: 0=off 1=events 2=+errors 3=+patch detail */
#define FKCAM_LOG 0
#define EF_NODRAW 0x00000080
#define EF_HIDE_MODEL 0x00000020
#define ET_PLAYER 1

#define SESS_KILLCAM_ENTITY(sess) ((sess).unk2)

/* ==========================================================================
 * Shared state
 * ========================================================================== */

static int s_victimClientNum = -1;
static qboolean s_scrRegistered = qfalse;
static int s_lastLogPatchMs[MAX_CLIENTS];

/* Entity-phase killcam tracking (spawn latch via snapshot entity patch). */
static int s_killEntitySpawnArchiveTime = -1;
static qboolean s_killcamTrackActive = qfalse;
static int s_killcamViewer = -1;
static int s_killcamEnt = -1;

/* ==========================================================================
 * Shared utilities
 * ========================================================================== */

static void GScr_SetFinalKillcamTargetEntity(void);
static void FK_EndBaseSession(void);
static void FK_EndProxySession(void);
static int FK_GetBodySlot(level_locals_t *level);

static void FK_Log(int level, const char *fmt, ...)
{
    char msg[384];
    va_list args;

    if(FKCAM_LOG < level)
    {
        return;
    }

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    Plugin_Printf("^3[%s]^7 %s\n", PLUGIN_NAME, msg);
}

static void FK_LogPatch(int viewerClientNum, const char *fmt, ...)
{
    char msg[384];
    va_list args;
    int nowMs;

    if(FKCAM_LOG < 3)
    {
        return;
    }

    if(viewerClientNum >= 0 && viewerClientNum < MAX_CLIENTS)
    {
        nowMs = Plugin_Milliseconds();
        if((nowMs - s_lastLogPatchMs[viewerClientNum]) < DEBUG_INTERVAL_MS)
        {
            return;
        }
        s_lastLogPatchMs[viewerClientNum] = nowMs;
    }

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    Plugin_Printf("^3[%s]^7 cl %d: %s\n", PLUGIN_NAME, viewerClientNum, msg);
}

static int FK_GetKillCamEntity(int viewerClientNum, const playerState_t *ps)
{
    int killCamEntity;

    killCamEntity = ps->killCamEntity;
    if(killCamEntity >= 0 && killCamEntity < KILLCAM_ENTITY_NONE)
    {
        return killCamEntity;
    }

    if(viewerClientNum < 0 || viewerClientNum >= MAX_CLIENTS)
    {
        return KILLCAM_ENTITY_NONE;
    }

    killCamEntity = SESS_KILLCAM_ENTITY(Plugin_GetLevelBase()->clients[viewerClientNum].sess);
    return (killCamEntity < 0) ? KILLCAM_ENTITY_NONE : killCamEntity;
}

static void FK_ResetKillcamSession(void)
{
    s_killEntitySpawnArchiveTime = -1;
    s_killcamTrackActive = qfalse;
    s_killcamViewer = -1;
    s_killcamEnt = -1;
}

static qboolean FK_KillcamTrackMatchesClient(client_t *client)
{
    int viewerClientNum;

    if(!s_killcamTrackActive || !client)
    {
        return qfalse;
    }

    viewerClientNum = Plugin_GetClientNumForClient(client);
    return viewerClientNum >= 0 && viewerClientNum == s_killcamViewer;
}

static void FK_NoteKillEntityInSnapshot(int killCamEnt, int archiveTime, const entityState_t *entState)
{
    if(!entState || killCamEnt < 0 || killCamEnt >= KILLCAM_ENTITY_NONE || archiveTime <= 0)
    {
        return;
    }

    if(entState->number != killCamEnt || entState->eType == ET_PLAYER)
    {
        return;
    }

    /* First snapshot inclusion = spawn moment (highest archiveTime seen). */
    if(s_killEntitySpawnArchiveTime < 0 || archiveTime > s_killEntitySpawnArchiveTime)
    {
        if(s_killEntitySpawnArchiveTime < 0)
        {
            FK_Log(1, "kill entity %d spawn archiveTime=%d (snapshot)", killCamEnt, archiveTime);
        }

        s_killEntitySpawnArchiveTime = archiveTime;
    }
}

static qboolean FK_IsEntityKillCamPovThisFrame(int archiveTime)
{
    if(archiveTime <= 0 || s_killcamEnt < 0 || s_killcamEnt >= KILLCAM_ENTITY_NONE)
    {
        return qfalse;
    }

    if(s_killEntitySpawnArchiveTime < 0)
    {
        return qfalse;
    }

    return archiveTime <= s_killEntitySpawnArchiveTime;
}

static qboolean FK_ShouldPatch(int viewerClientNum, const playerState_t *ps,
    level_locals_t *level, int *victimOut, int *killCamEntOut)
{
    int victim;
    int killCamEnt;

    victim = s_victimClientNum;
    if(victim < 0 || victim >= level->maxclients)
    {
        return qfalse;
    }
    if(viewerClientNum < 0 || viewerClientNum >= level->maxclients)
    {
        return qfalse;
    }
    if(viewerClientNum == victim)
    {
        return qfalse;
    }

    killCamEnt = FK_GetKillCamEntity(viewerClientNum, ps);
    if(killCamEnt < 0 || killCamEnt >= KILLCAM_ENTITY_NONE)
    {
        return qfalse;
    }

    *victimOut = victim;
    *killCamEntOut = killCamEnt;
    return qtrue;
}

static void FK_RegisterScrFunction(void)
{
    if(s_scrRegistered)
    {
        return;
    }

    Plugin_ScrAddFunction("SetFinalKillcamTargetEntity", GScr_SetFinalKillcamTargetEntity);
    s_scrRegistered = qtrue;
    FK_Log(1, "SetFinalKillcamTargetEntity registered");
}

static void GScr_SetFinalKillcamTargetEntity(void)
{
    int entNum;
    level_locals_t *level;

    if(Plugin_Scr_GetNumParam() != 1)
    {
        Plugin_Scr_Error("SetFinalKillcamTargetEntity( <entityNum> ) requires one argument");
    }

    entNum = Plugin_Scr_GetInt(0);
    if(entNum < 0)
    {
        s_victimClientNum = -1;
        FK_EndBaseSession();
        FK_EndProxySession();
        FK_ResetKillcamSession();
        FK_Log(1, "victim cleared");
        return;
    }

    if(s_victimClientNum != entNum)
    {
        FK_EndBaseSession();
        FK_EndProxySession();
        FK_ResetKillcamSession();
    }

    level = Plugin_GetLevelBase();
    if(!level)
    {
        Plugin_Scr_Error("SetFinalKillcamTargetEntity: level not ready");
    }
    if(entNum >= level->maxclients)
    {
        Plugin_Scr_Error("SetFinalKillcamTargetEntity: entity number out of range");
    }

    s_victimClientNum = entNum;
    FK_Log(1, "victim set to %d bodySlot=%d", entNum, FK_GetBodySlot(level));
}

/* ==========================================================================
 * Block 1: Base logic — origin spoofing & hide viewer model
 * ========================================================================== */

static int s_snapViewer = -1;
static qboolean s_snapActive = qfalse;
static float s_snapAimOrigin[3];

static qboolean FK_SnapMatchesClient(client_t *client)
{
    int viewerClientNum;

    if(!s_snapActive || !client)
    {
        return qfalse;
    }

    viewerClientNum = Plugin_GetClientNumForClient(client);
    return viewerClientNum >= 0 && viewerClientNum == s_snapViewer;
}

static void FK_EndBaseSession(void)
{
    s_snapViewer = -1;
    s_snapActive = qfalse;
    s_snapAimOrigin[0] = 0.0f;
    s_snapAimOrigin[1] = 0.0f;
    s_snapAimOrigin[2] = 0.0f;
}

static void FK_BeginBaseSession(int viewerClientNum, const float aimOrigin[3])
{
    s_snapViewer = viewerClientNum;
    s_snapActive = qtrue;
    s_snapAimOrigin[0] = aimOrigin[0];
    s_snapAimOrigin[1] = aimOrigin[1];
    s_snapAimOrigin[2] = aimOrigin[2];
}

static void FK_HidePsModel(playerState_t *ps)
{
    ps->eFlags |= EF_NODRAW | EF_HIDE_MODEL;
}

static void FK_HideViewerClientState(clientState_t *cs)
{
    int i;

    if(!cs)
    {
        return;
    }

    cs->modelindex = 0;
    for(i = 0; i < 6; ++i)
    {
        cs->attachModelIndex[i] = 0;
        cs->attachTagIndex[i] = 0;
    }
}

static void FK_FreezeTrajectory(trajectory_t *tr, const float base[3])
{
    if(!tr)
    {
        return;
    }

    tr->trType = TR_STATIONARY;
    tr->trTime = 0;
    tr->trDuration = 0;
    tr->trBase[0] = base[0];
    tr->trBase[1] = base[1];
    tr->trBase[2] = base[2];
    tr->trDelta[0] = 0.0f;
    tr->trDelta[1] = 0.0f;
    tr->trDelta[2] = 0.0f;
}

static void FK_FreezeEntityLerp(entityState_t *entState)
{
    if(!entState)
    {
        return;
    }

    FK_FreezeTrajectory(&entState->lerp.pos, entState->lerp.pos.trBase);
    FK_FreezeTrajectory(&entState->lerp.apos, entState->lerp.apos.trBase);
}

static void FK_HideViewerEntityAtAim(entityState_t *entState)
{
    entState->lerp.pos.trBase[0] = s_snapAimOrigin[0];
    entState->lerp.pos.trBase[1] = s_snapAimOrigin[1];
    entState->lerp.pos.trBase[2] = s_snapAimOrigin[2];
    FK_FreezeEntityLerp(entState);
    entState->lerp.eFlags |= EF_NODRAW | EF_HIDE_MODEL;
}

static void FK_ApplyOriginSpoof(playerState_t *ps, const float victimOrigin[3])
{
    ps->origin[0] = victimOrigin[0];
    ps->origin[1] = victimOrigin[1];
    ps->origin[2] = victimOrigin[2];
    FK_HidePsModel(ps);
}

static qboolean FK_PatchBaseEntity(client_t *client, entityState_t *entState)
{
    if(!s_snapActive || !entState || !FK_SnapMatchesClient(client))
    {
        return qfalse;
    }

    if(entState->number != s_snapViewer)
    {
        return qfalse;
    }

    FK_HideViewerEntityAtAim(entState);
    return qtrue;
}

static qboolean FK_PatchBaseClientState(client_t *client, clientState_t *cs, int csClientIndex)
{
    if(!s_snapActive || !cs || !FK_SnapMatchesClient(client))
    {
        return qfalse;
    }

    if(csClientIndex != s_snapViewer)
    {
        return qfalse;
    }

    FK_HideViewerClientState(cs);
    return qtrue;
}

/* ==========================================================================
 * Block 2: Proxy model — viewer body on reserved client slot
 * ========================================================================== */

static int s_snapBodySlot = -1;
static qboolean s_snapHasProxy = qfalse;
static qboolean s_hasBodyCs = qfalse;
static qboolean s_snapBodyInList = qfalse;
static qboolean s_snapBodyCsInList = qfalse;
static qboolean s_loggedBodyInject = qfalse;
static entityState_t s_snapProxyEnt;
static clientState_t s_bodyCs;

static qboolean FK_BuildProxyEntity(int viewerClientNum, int bodySlot, int proxyArchiveTime,
    int fallbackArchiveTime, entityState_t *entState);
static qboolean FK_RefreshBodyProxy(int viewerClientNum, int bodySlot, int archiveTime);

static int FK_GetBodySlot(level_locals_t *level)
{
    if(!level || level->maxclients <= 0)
    {
        return 0;
    }

    return level->maxclients - 1;
}

static qboolean FK_IsBodySlotValid(int bodySlot, int viewerClientNum, int victimClientNum, level_locals_t *level)
{
    if(bodySlot < 0 || bodySlot >= level->maxclients)
    {
        return qfalse;
    }
    if(bodySlot == viewerClientNum || bodySlot == victimClientNum)
    {
        return qfalse;
    }
    return qtrue;
}

static void FK_EndProxySession(void)
{
    s_snapHasProxy = qfalse;
    s_hasBodyCs = qfalse;
    s_snapBodyInList = qfalse;
    s_snapBodyCsInList = qfalse;
    s_snapBodySlot = -1;
    s_loggedBodyInject = qfalse;
    memset(&s_snapProxyEnt, 0, sizeof(s_snapProxyEnt));
    memset(&s_bodyCs, 0, sizeof(s_bodyCs));
}

static void FK_BeginProxySession(int viewerClientNum, int victimClientNum, int archiveTime,
    level_locals_t *level)
{
    client_t *bodyClient;

    s_snapBodyInList = qfalse;
    s_snapBodyCsInList = qfalse;
    s_snapBodySlot = FK_GetBodySlot(level);

    if(!FK_IsBodySlotValid(s_snapBodySlot, viewerClientNum, victimClientNum, level))
    {
        FK_Log(2, "body slot %d invalid for viewer=%d victim=%d",
            s_snapBodySlot, viewerClientNum, victimClientNum);
        return;
    }

    bodyClient = Plugin_GetClientForClientNum(s_snapBodySlot);
    if(bodyClient && bodyClient->state >= CS_PRIMED)
    {
        FK_Log(2, "body slot %d occupied by connected client; reserve it for killcam bodies",
            s_snapBodySlot);
    }

    FK_RefreshBodyProxy(viewerClientNum, s_snapBodySlot, archiveTime);
}

static qboolean FK_ProxyActive(void)
{
    return s_snapHasProxy;
}

static void FK_AssignBodyProxySlot(entityState_t *entState, int bodySlot)
{
    entState->number = bodySlot;
    entState->clientNum = bodySlot;
    entState->lerp.eFlags &= ~(EF_NODRAW | EF_HIDE_MODEL);
    if(entState->eType != ET_PLAYER)
    {
        entState->eType = ET_PLAYER;
    }
    entState->groundEntityNum = KILLCAM_ENTITY_NONE;
    entState->solid = 0;
    entState->iHeadIcon = 0;
    entState->iHeadIconTeam = 0;
}

static void FK_HideBodyProxyClientState(clientState_t *cs)
{
    if(!cs)
    {
        return;
    }

    /* rank 0 still draws as level 1 on the client; use invalid rank so lookups fail */
    cs->rank = FK_PROXY_RANK_HIDDEN;
    cs->prestige = FK_PROXY_RANK_HIDDEN;
}

static void FK_FinalizeBodyProxy(entityState_t *entState, int bodySlot)
{
    FK_AssignBodyProxySlot(entState, bodySlot);
}

static qboolean FK_LatchBodyClientState(int viewerClientNum, int proxyArchiveTime, int fallbackArchiveTime)
{
    if(Plugin_SV_GetArchivedClientInfo(viewerClientNum, proxyArchiveTime, NULL, &s_bodyCs, NULL))
    {
        s_hasBodyCs = qtrue;
        return qtrue;
    }

    if(fallbackArchiveTime > 0 && fallbackArchiveTime != proxyArchiveTime
        && Plugin_SV_GetArchivedClientInfo(viewerClientNum, fallbackArchiveTime, NULL, &s_bodyCs, NULL))
    {
        s_hasBodyCs = qtrue;
        return qtrue;
    }

    if(Plugin_SV_GetArchivedClientState(viewerClientNum, proxyArchiveTime, &s_bodyCs))
    {
        s_hasBodyCs = qtrue;
        return qtrue;
    }

    if(fallbackArchiveTime > 0 && fallbackArchiveTime != proxyArchiveTime
        && Plugin_SV_GetArchivedClientState(viewerClientNum, fallbackArchiveTime, &s_bodyCs))
    {
        s_hasBodyCs = qtrue;
        return qtrue;
    }

    if(Plugin_SV_GetClientState(viewerClientNum, &s_bodyCs))
    {
        s_hasBodyCs = qtrue;
        FK_Log(2, "proxy body: using live clientState for cl=%d", viewerClientNum);
        return qtrue;
    }

    FK_Log(2, "proxy body: archived clientState missing cl=%d proxyTime=%d replay=%d",
        viewerClientNum, proxyArchiveTime, fallbackArchiveTime);
    return qfalse;
}

static qboolean FK_BuildProxyFromPs(int viewerClientNum, int bodySlot, int archiveTime,
    entityState_t *entState)
{
    playerState_t archivedPs;

    if(!Plugin_SV_GetArchivedClientInfo(viewerClientNum, archiveTime, &archivedPs, NULL, NULL))
    {
        return qfalse;
    }

    memset(entState, 0, sizeof(*entState));
    entState->eType = ET_PLAYER;
    entState->number = viewerClientNum;
    entState->clientNum = viewerClientNum;
    entState->lerp.pos.trBase[0] = archivedPs.origin[0];
    entState->lerp.pos.trBase[1] = archivedPs.origin[1];
    entState->lerp.pos.trBase[2] = archivedPs.origin[2];
    entState->lerp.apos.trBase[0] = archivedPs.viewangles[0];
    entState->lerp.apos.trBase[1] = archivedPs.viewangles[1];
    entState->lerp.apos.trBase[2] = archivedPs.viewangles[2];
    entState->lerp.eFlags = archivedPs.eFlags & ~(EF_NODRAW | EF_HIDE_MODEL);
    entState->legsAnim = archivedPs.legsAnim;
    entState->torsoAnim = archivedPs.torsoAnim;
    entState->weapon = archivedPs.weapon;
    FK_FinalizeBodyProxy(entState, bodySlot);
    return qtrue;
}

static qboolean FK_BuildProxyEntity(int viewerClientNum, int bodySlot, int proxyArchiveTime,
    int fallbackArchiveTime, entityState_t *entState)
{
    if(Plugin_SV_GetArchivedClientEntityState(viewerClientNum, proxyArchiveTime, entState))
    {
        FK_FinalizeBodyProxy(entState, bodySlot);
        return qtrue;
    }

    if(fallbackArchiveTime > 0 && fallbackArchiveTime != proxyArchiveTime
        && Plugin_SV_GetArchivedClientEntityState(viewerClientNum, fallbackArchiveTime, entState))
    {
        FK_FinalizeBodyProxy(entState, bodySlot);
        return qtrue;
    }

    if(FK_BuildProxyFromPs(viewerClientNum, bodySlot, proxyArchiveTime, entState))
    {
        return qtrue;
    }

    if(fallbackArchiveTime > 0 && fallbackArchiveTime != proxyArchiveTime)
    {
        return FK_BuildProxyFromPs(viewerClientNum, bodySlot, fallbackArchiveTime, entState);
    }

    return qfalse;
}

static qboolean FK_RefreshBodyProxy(int viewerClientNum, int bodySlot, int archiveTime)
{
    if(!FK_BuildProxyEntity(viewerClientNum, bodySlot, archiveTime, archiveTime, &s_snapProxyEnt))
    {
        s_snapHasProxy = qfalse;
        FK_Log(2, "proxy body: no archived data cl=%d archiveTime=%d",
            viewerClientNum, archiveTime);
        return qfalse;
    }

    FK_LatchBodyClientState(viewerClientNum, archiveTime, archiveTime);
    if(s_hasBodyCs)
    {
        s_bodyCs.clientIndex = bodySlot;
    }

    s_snapHasProxy = qtrue;
    return qtrue;
}

static void FK_ApplyBodyClientState(clientState_t *cs, int bodySlot)
{
    if(!cs || !s_hasBodyCs)
    {
        return;
    }

    *cs = s_bodyCs;
    cs->clientIndex = bodySlot;
    FK_HideBodyProxyClientState(cs);
}

static qboolean FK_PatchProxyEntity(client_t *client, entityState_t *entState)
{
    if(!FK_ProxyActive() || !entState || !FK_SnapMatchesClient(client))
    {
        return qfalse;
    }

    if(entState->number != s_snapBodySlot)
    {
        return qfalse;
    }

    s_snapBodyInList = qtrue;
    *entState = s_snapProxyEnt;
    FK_AssignBodyProxySlot(entState, s_snapBodySlot);
    return qtrue;
}

static qboolean FK_PatchProxyOwnEntity(client_t *client, entityState_t *entState)
{
    if(!FK_ProxyActive() || !entState || !FK_SnapMatchesClient(client))
    {
        return qfalse;
    }

    if(s_snapBodyInList)
    {
        return qfalse;
    }

    *entState = s_snapProxyEnt;
    FK_AssignBodyProxySlot(entState, s_snapBodySlot);
    if(!s_loggedBodyInject)
    {
        s_loggedBodyInject = qtrue;
        FK_Log(2, "proxy body: inject entity slot=%d num=%d hasCs=%d modelindex=%d",
            s_snapBodySlot, entState->number, s_hasBodyCs, s_hasBodyCs ? s_bodyCs.modelindex : 0);
    }
    return qtrue;
}

static qboolean FK_PatchProxyClientState(client_t *client, clientState_t *cs, int csClientIndex)
{
    if(!FK_ProxyActive() || !s_hasBodyCs || !cs || !FK_SnapMatchesClient(client))
    {
        return qfalse;
    }

    if(csClientIndex == s_snapViewer)
    {
        FK_HideViewerClientState(cs);
        return qtrue;
    }

    if(csClientIndex != s_snapBodySlot)
    {
        return qfalse;
    }

    s_snapBodyCsInList = qtrue;
    FK_ApplyBodyClientState(cs, s_snapBodySlot);
    return qtrue;
}

static qboolean FK_PatchProxyOwnClientState(client_t *client, clientState_t *cs)
{
    if(!FK_ProxyActive() || !s_hasBodyCs || !cs || !FK_SnapMatchesClient(client))
    {
        return qfalse;
    }

    if(s_snapBodyCsInList || s_snapBodySlot < 0)
    {
        return qfalse;
    }

    FK_ApplyBodyClientState(cs, s_snapBodySlot);
    if(!s_loggedBodyInject)
    {
        s_loggedBodyInject = qtrue;
        FK_Log(2, "proxy body: inject clientState slot=%d modelindex=%d",
            s_snapBodySlot, cs->modelindex);
    }
    return qtrue;
}

/* ==========================================================================
 * Snapshot patch callbacks
 * ========================================================================== */

static qboolean FK_PatchPlayerState(client_t *client, playerState_t *ps, int archiveTime)
{
    int viewerClientNum;
    int victimClientNum;
    int killCamEnt;
    float victimOrigin[3];
    level_locals_t *level;

    FK_EndBaseSession();
    FK_EndProxySession();
    s_killcamTrackActive = qfalse;
    s_killcamViewer = -1;
    s_killcamEnt = -1;

    if(!client || !ps)
    {
        FK_Log(2, "patch abort: null client or playerState");
        return qfalse;
    }

    viewerClientNum = Plugin_GetClientNumForClient(client);
    if(viewerClientNum < 0 || viewerClientNum >= MAX_CLIENTS)
    {
        FK_Log(2, "patch abort: bad viewerClientNum %d", viewerClientNum);
        return qfalse;
    }

    FK_LogPatch(viewerClientNum, "patch enter archiveTime=%d victim=%d",
        archiveTime, s_victimClientNum);

    level = Plugin_GetLevelBase();
    if(!level || !FK_ShouldPatch(viewerClientNum, ps, level, &victimClientNum, &killCamEnt))
    {
        return qfalse;
    }

    if(archiveTime <= 0)
    {
        return qfalse;
    }

    s_killcamTrackActive = qtrue;
    s_killcamViewer = viewerClientNum;
    s_killcamEnt = killCamEnt;

    if(!FK_IsEntityKillCamPovThisFrame(archiveTime))
    {
        return qfalse;
    }

    if(!Plugin_SV_GetArchivedClientOrigin(victimClientNum, archiveTime, victimOrigin))
    {
        FK_Log(2, "cl %d: SV_GetArchivedClientOrigin failed victim=%d archiveTime=%d",
            viewerClientNum, victimClientNum, archiveTime);
        return qfalse;
    }

    FK_BeginBaseSession(viewerClientNum, victimOrigin);
    FK_BeginProxySession(viewerClientNum, victimClientNum, archiveTime, level);
    FK_ApplyOriginSpoof(ps, victimOrigin);

    if(s_snapHasProxy)
    {
        FK_LogPatch(viewerClientNum,
            "PATCH ok victim=%d killCamEntity=%d aim=(%.0f %.0f %.0f) bodySlot=%d body=(%.0f %.0f %.0f) replayTime=%d",
            victimClientNum, killCamEnt,
            victimOrigin[0], victimOrigin[1], victimOrigin[2],
            s_snapBodySlot,
            s_snapProxyEnt.lerp.pos.trBase[0], s_snapProxyEnt.lerp.pos.trBase[1],
            s_snapProxyEnt.lerp.pos.trBase[2], archiveTime);
    }
    else
    {
        FK_LogPatch(viewerClientNum,
            "PATCH ok victim=%d killCamEntity=%d aim=(%.0f %.0f %.0f) body proxy unavailable",
            victimClientNum, killCamEnt,
            victimOrigin[0], victimOrigin[1], victimOrigin[2]);
    }

    return qtrue;
}

static qboolean FK_PatchEntity(client_t *client, playerState_t *ps, entityState_t *entState, int archiveTime,
    snapshotPatchMode_t mode)
{
    if(mode == SNAPSHOT_PATCH_MODIFY)
    {
        if(FK_KillcamTrackMatchesClient(client) && entState)
        {
            FK_NoteKillEntityInSnapshot(s_killcamEnt, archiveTime, entState);
        }

        if(FK_PatchBaseEntity(client, entState))
        {
            return qtrue;
        }

        return FK_PatchProxyEntity(client, entState);
    }

    (void)ps;
    (void)archiveTime;
    return FK_PatchProxyOwnEntity(client, entState);
}

static qboolean FK_PatchClientState(client_t *client, playerState_t *ps, clientState_t *cs,
    int csClientIndex, int archiveTime, snapshotPatchMode_t mode)
{
    (void)ps;
    (void)archiveTime;

    if(mode == SNAPSHOT_PATCH_MODIFY)
    {
        if(FK_ProxyActive() && FK_SnapMatchesClient(client))
        {
            return FK_PatchProxyClientState(client, cs, csClientIndex);
        }

        return FK_PatchBaseClientState(client, cs, csClientIndex);
    }

    (void)csClientIndex;
    return FK_PatchProxyOwnClientState(client, cs);
}

/* ==========================================================================
 * Plugin lifecycle
 * ========================================================================== */

PCL int OnInit(void)
{
    Plugin_RegisterSnapshotPlayerStatePatch(FK_PatchPlayerState);
    Plugin_RegisterSnapshotEntityPatch(FK_PatchEntity);
    Plugin_RegisterSnapshotClientStatePatch(FK_PatchClientState);
    FK_RegisterScrFunction();

    Plugin_Printf("^2[%s]^7 loaded. Body proxy uses client slot ^3(maxclients - 1)^7.\n", PLUGIN_NAME);
    Plugin_Printf("^2[%s]^7 GSC: ^3SetFinalKillcamTargetEntity( victim getEntityNumber() )^7\n", PLUGIN_NAME);
    FK_Log(1, "init complete");
    return 0;
}

PCL void OnSpawnServer(void)
{
    FK_RegisterScrFunction();
}

PCL void OnUnload(void)
{
    s_victimClientNum = -1;
    s_scrRegistered = qfalse;
    FK_EndBaseSession();
    FK_EndProxySession();
    FK_ResetKillcamSession();
    Plugin_UnregisterSnapshotPlayerStatePatch();
    Plugin_UnregisterSnapshotEntityPatch();
    Plugin_UnregisterSnapshotClientStatePatch();
}

PCL void OnInfoRequest(pluginInfo_t *info)
{
    info->handlerVersion.major = PLUGIN_HANDLER_VERSION_MAJOR;
    info->handlerVersion.minor = PLUGIN_HANDLER_VERSION_MINOR;
    info->pluginVersion.major = PLUGIN_VER_MAJ;
    info->pluginVersion.minor = PLUGIN_VER_MIN;
    strncpy(info->fullName, PLUGIN_NAME, sizeof(info->fullName));
    strncpy(info->shortDescription, PLUGIN_DESC, sizeof(info->shortDescription));
    strncpy(info->longDescription, PLUGIN_DESC_LONG, sizeof(info->longDescription));
}
