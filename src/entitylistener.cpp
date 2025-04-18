/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2025 Source2ZE
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "entitylistener.h"
#include "common.h"
#include "cs2_sdk/entity/cbaseentity.h"
#include "cs2fixes.h"
#include "entities.h"
#include "entity/cgamerules.h"
#include "entwatch.h"
#include "gameconfig.h"
#include "plat.h"

extern CGameConfig* g_GameConfig;
extern CCSGameRules* g_pGameRules;

CConVar<bool> g_cvarGrenadeNoBlock("cs2f_noblock_grenades", FCVAR_NONE, "Whether to use noblock on grenade projectiles", false);

void Patch_GetHammerUniqueId(CEntityInstance* pEntity)
{
	static int offset = g_GameConfig->GetOffset("GetHammerUniqueId");
	void** vtable = *(void***)pEntity;

	// xor al, al -> mov al, 1
	// so it always returns true and allows hammerid to be copied into the schema prop
	Plat_WriteMemory(vtable[offset], (uint8_t*)"\xB0\x01", 2);
}

void CEntityListener::OnEntitySpawned(CEntityInstance* pEntity)
{
#ifdef _DEBUG
	const char* pszClassName = pEntity->m_pEntity->m_designerName.String();
	Message("Entity spawned: %s %s\n", pszClassName, ((CBaseEntity*)pEntity)->m_sUniqueHammerID().Get());
#endif

	if (g_cvarGrenadeNoBlock.Get() && V_stristr(pEntity->GetClassname(), "_projectile"))
		reinterpret_cast<CBaseEntity*>(pEntity)->SetCollisionGroup(COLLISION_GROUP_DEBRIS);

	EntityHandler_OnEntitySpawned(reinterpret_cast<CBaseEntity*>(pEntity));

	if (g_cvarEnableEntWatch.Get())
		EW_OnEntitySpawned(pEntity);
}

void CEntityListener::OnEntityCreated(CEntityInstance* pEntity)
{
	ExecuteOnce(Patch_GetHammerUniqueId(pEntity));

	if (!V_strcmp("cs_gamerules", pEntity->GetClassname()))
		g_pGameRules = ((CCSGameRulesProxy*)pEntity)->m_pGameRules;
}

void CEntityListener::OnEntityDeleted(CEntityInstance* pEntity)
{
	EW_OnEntityDeleted(pEntity);
}

void CEntityListener::OnEntityParentChanged(CEntityInstance* pEntity, CEntityInstance* pNewParent)
{
}