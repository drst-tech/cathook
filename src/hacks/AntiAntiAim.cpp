/*
 * Created on 29.07.18.
 */

#include "common.hpp"
#include "hacks/AntiAntiAim.hpp"
#include "sdk/dt_recv_redef.h"

namespace hacks::shared::anti_anti_aim
{
static settings::Boolean enable{ "anti-anti-aim.enable", "false" };
static settings::Boolean debug{ "anti-anti-aim.debug.enable", "false" };

std::unordered_map<unsigned, brutedata> resolver_map;

static inline void modifyAnlges()
{
    for (int i = 1; i <= g_IEngine->GetMaxClients(); i++)
    {
        auto player = ENTITY(i);
        if (CE_BAD(player) || !player->m_bAlivePlayer() || !player->m_bEnemy() || !player->player_info.friendsID)
            continue;
        auto &data  = resolver_map[player->player_info.friendsID];
        auto &angle = CE_VECTOR(player, netvar.m_angEyeAngles);
        angle.x     = data.new_angle.x;
        angle.y     = data.new_angle.y;
    }
}

void frameStageNotify(ClientFrameStage_t stage)
{
#if !ENABLE_TEXTMODE
    if (!enable || !g_IEngine->IsInGame())
        return;
    if (stage == FRAME_NET_UPDATE_POSTDATAUPDATE_START)
    {
        modifyAnlges();
    }
#endif
}

static std::array<float, 5> yaw_resolves{ 0.0f, 180.0f, 65.0f, -65.0f, -180.0f };

static float resolveAngleYaw(float angle, brutedata &brute)
{
    brute.original_angle.y = angle;
    while (angle > 180)
        angle -= 360;

    while (angle < -180)
        angle += 360;

    // Yaw Resolving
    // Find out which angle we should try
    int entry = (int) std::floor((brute.brutenum / 2.0f)) % yaw_resolves.size();
    angle += yaw_resolves[entry];

    while (angle > 180)
        angle -= 360;

    while (angle < -180)
        angle += 360;
    brute.new_angle.y = angle;
    return angle;
}

static float resolveAnglePitch(float angle, brutedata &brute)
{
    brute.original_angle.x = angle;
    if (brute.brutenum % 2)
    {
        // Pitch resolver
        if (angle >= 90)
            angle = -89;
        if (angle <= -90)
            angle = 89;
    }
    brute.new_angle.x = angle;
    return angle;
}

void increaseBruteNum(int idx)
{
    auto ent = ENTITY(idx);
    if (CE_BAD(ent) || !ent->player_info.friendsID)
        return;
    auto &data = hacks::shared::anti_anti_aim::resolver_map[ent->player_info.friendsID];
    if (data.hits_in_a_row >= 4)
        data.hits_in_a_row = 2;
    else if (data.hits_in_a_row >= 2)
        data.hits_in_a_row = 0;
    else
    {
        data.brutenum++;
        if (debug)
            logging::Info("AAA: Brutenum for entity %i increased to %i", idx, data.brutenum);
        data.hits_in_a_row = 0;
        auto &angle        = CE_VECTOR(ent, netvar.m_angEyeAngles);
        angle.x            = resolveAnglePitch(data.original_angle.x, data);
        angle.y            = resolveAngleYaw(data.original_angle.y, data);
        data.new_angle.x   = angle.x;
        data.new_angle.y   = angle.y;
    }
}

static void pitchHook(const CRecvProxyData *pData, void *pStruct, void *pOut)
{
    float flPitch      = pData->m_Value.m_Float;
    float *flPitch_out = (float *) pOut;

    if (!enable)
    {
        *flPitch_out = flPitch;
        return;
    }

    auto client_ent   = (IClientEntity *) (pStruct);
    CachedEntity *ent = ENTITY(client_ent->entindex());
    if (CE_GOOD(ent))
        *flPitch_out = resolveAnglePitch(flPitch, resolver_map[ent->player_info.friendsID]);
}

static void yawHook(const CRecvProxyData *pData, void *pStruct, void *pOut)
{
    float flYaw      = pData->m_Value.m_Float;
    float *flYaw_out = (float *) pOut;

    if (!enable)
    {
        *flYaw_out = flYaw;
        return;
    }

    auto client_ent   = (IClientEntity *) (pStruct);
    CachedEntity *ent = ENTITY(client_ent->entindex());
    if (CE_GOOD(ent))
        *flYaw_out = resolveAngleYaw(flYaw, resolver_map[ent->player_info.friendsID]);
}

// *_ptr points to what we need to modify while *_ProxyFn holds the old value
static RecvVarProxyFn *original_ptrX;
static RecvVarProxyFn original_ProxyFnX;
static RecvVarProxyFn *original_ptrY;
static RecvVarProxyFn original_ProxyFnY;

static void hook()
{
    auto pClass = g_IBaseClient->GetAllClasses();
    while (pClass)
    {
        const char *pszName = pClass->m_pRecvTable->m_pNetTableName;
        // "DT_TFPlayer", "tfnonlocaldata"
        if (!strcmp(pszName, "DT_TFPlayer"))
        {
            for (int i = 0; i < pClass->m_pRecvTable->m_nProps; i++)
            {
                RecvPropRedef *pProp1 = (RecvPropRedef *) &(pClass->m_pRecvTable->m_pProps[i]);
                if (!pProp1)
                    continue;
                const char *pszName2 = pProp1->m_pVarName;
                if (!strcmp(pszName2, "tfnonlocaldata"))
                    for (int j = 0; j < pProp1->m_pDataTable->m_nProps; j++)
                    {
                        RecvPropRedef *pProp2 = (RecvPropRedef *) &(pProp1->m_pDataTable->m_pProps[j]);
                        if (!pProp2)
                            continue;
                        const char *name = pProp2->m_pVarName;

                        // Pitch Fix
                        if (!strcmp(name, "m_angEyeAngles[0]"))
                        {
                            original_ptrX     = &pProp2->m_ProxyFn;
                            original_ProxyFnX = pProp2->m_ProxyFn;
                            pProp2->m_ProxyFn = pitchHook;
                        }

                        // Yaw Fix
                        if (!strcmp(name, "m_angEyeAngles[1]"))
                        {
                            original_ptrY = &pProp2->m_ProxyFn;
                            logging::Info("Yaw Fix Applied");
                            original_ProxyFnY = pProp2->m_ProxyFn;
                            pProp2->m_ProxyFn = yawHook;
                        }
                    }
            }
        }
        pClass = pClass->m_pNext;
    }
}

static void shutdown()
{
    *original_ptrX = original_ProxyFnX;
    *original_ptrY = original_ProxyFnY;
}

static InitRoutine init([]() {
    hook();
    EC::Register(EC::Shutdown, shutdown, "antiantiaim_shutdown");
#if ENABLE_TEXTMODE
    EC::Register(EC::CreateMove, modifyAnlges, "cm_antiantiaim");
#endif
});
} // namespace hacks::shared::anti_anti_aim
