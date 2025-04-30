/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2025 Source2ZE
 * =============================================================================
 *
 * 僵尸重生(Zombie Reborn)模式实现文件
 * 这个文件实现了CS2中的僵尸模式,包括:
 * - 玩家感染系统:将人类玩家转变为僵尸
 * - 母体僵尸选择系统:从人类中随机选择初始僵尸
 * - 玩家类别系统:不同类型的人类和僵尸角色
 * - 击退系统:人类武器对僵尸造成的击退效果
 * - 回合管理:检查胜利条件、结束回合、计分等
 * 
 * 本程序是自由软件;您可以根据自由软件基金会发布的GNU通用公共许可证(版本3.0)
 * 重新分发和/或修改它。
 *
 * 本程序的分发是希望它是有用的,但没有任何担保;甚至没有对适销性或特定用途适用性的暗示担保。
 * 有关更多详细信息,请参阅GNU通用公共许可证。
 *
 * 您应该已经收到GNU通用公共许可证的副本。如果没有,请参阅<http://www.gnu.org/licenses/>。
 */

#include "usermessages.pb.h"

#include "commands.h"
#include "ctimer.h"
#include "customio.h"
#include "engine/igameeventsystem.h"
#include "entity/cgamerules.h"
#include "entity/cparticlesystem.h"
#include "entity/cteam.h"
#include "entity/services.h"
#include "eventlistener.h"
#include "leader.h"
#include "networksystem/inetworkmessages.h"
#include "playermanager.h"
#include "recipientfilters.h"
#include "serversideclient.h"
#include "tier0/vprof.h"
#include "user_preferences.h"
#include "utils/entity.h"
#include "vendor/nlohmann/json.hpp"
#include "zombiereborn.h"
#include <fstream>
#include <sstream>

#include "tier0/memdbgon.h"

using ordered_json = nlohmann::ordered_json;

extern CGameEntitySystem* g_pEntitySystem;
extern IVEngineServer2* g_pEngineServer2;
extern CGlobalVars* GetGlobals();
extern CCSGameRules* g_pGameRules;
extern IGameEventManager2* g_gameEventManager;
extern IGameEventSystem* g_gameEventSystem;
extern double g_flUniversalTime;

void ZR_Infect(CCSPlayerController* pAttackerController, CCSPlayerController* pVictimController, bool bBroadcast);
bool ZR_CheckTeamWinConditions(int iTeamNum);
void ZR_Cure(CCSPlayerController* pTargetController);
void ZR_EndRoundAndAddTeamScore(int iTeamNum);
void SetupCTeams();
bool ZR_IsTeamAlive(int iTeamNum);

EZRRoundState g_ZRRoundState = EZRRoundState::ROUND_START;
static int g_iInfectionCountDown = 0;
static bool g_bRespawnEnabled = true;
static CHandle<CBaseEntity> g_hRespawnToggler;
static CHandle<CTeam> g_hTeamCT;
static CHandle<CTeam> g_hTeamT;

CZRPlayerClassManager* g_pZRPlayerClassManager = nullptr;
ZRWeaponConfig* g_pZRWeaponConfig = nullptr;
ZRHitgroupConfig* g_pZRHitgroupConfig = nullptr;

CConVar<bool> g_cvarEnableZR("zr_enable", FCVAR_NONE, "Whether to enable ZR features", false);
CConVar<float> g_cvarMaxZteleDistance("zr_ztele_max_distance", FCVAR_NONE, "Maximum distance players are allowed to move after starting ztele", 150.0f, true, 0.0f, false, 0.0f);
CConVar<bool> g_cvarZteleHuman("zr_ztele_allow_humans", FCVAR_NONE, "Whether to allow humans to use ztele", false);
CConVar<float> g_cvarKnockbackScale("zr_knockback_scale", FCVAR_NONE, "Global knockback scale", 5.0f);
CConVar<int> g_cvarInfectSpawnType("zr_infect_spawn_type", FCVAR_NONE, "Type of Mother Zombies Spawn [0 = MZ spawn where they stand, 1 = MZ get teleported back to spawn on being picked]", (int)EZRSpawnType::RESPAWN, true, 0, true, 1);
CConVar<int> g_cvarInfectSpawnTimeMin("zr_infect_spawn_time_min", FCVAR_NONE, "Minimum time in which Mother Zombies should be picked, after round start", 15, true, 0, false, 0);
CConVar<int> g_cvarInfectSpawnTimeMax("zr_infect_spawn_time_max", FCVAR_NONE, "Maximum time in which Mother Zombies should be picked, after round start", 15, true, 1, false, 0);
CConVar<int> g_cvarInfectSpawnMZRatio("zr_infect_spawn_mz_ratio", FCVAR_NONE, "Ratio of all Players to Mother Zombies to be spawned at round start", 7, true, 1, true, 64);
CConVar<int> g_cvarInfectSpawnMinCount("zr_infect_spawn_mz_min_count", FCVAR_NONE, "Minimum amount of Mother Zombies to be spawned at round start", 1, true, 0, false, 0);
CConVar<float> g_cvarRespawnDelay("zr_respawn_delay", FCVAR_NONE, "Time before a zombie is automatically respawned, -1 disables this. Note that maps can still manually respawn at any time", 5.0f, true, -1.0f, false, 0.0f);
CConVar<int> g_cvarDefaultWinnerTeam("zr_default_winner_team", FCVAR_NONE, "Which team wins when time ran out [1 = Draw, 2 = Zombies, 3 = Humans]", CS_TEAM_SPECTATOR, true, 1, true, 3);
CConVar<int> g_cvarMZImmunityReduction("zr_mz_immunity_reduction", FCVAR_NONE, "How much mz immunity to reduce for each player per round (0-100)", 20, true, 0, true, 100);
CConVar<int> g_cvarGroanChance("zr_sounds_groan_chance", FCVAR_NONE, "How likely should a zombie groan whenever they take damage (1 / N)", 5, true, 1, false, 0);
CConVar<float> g_cvarMoanInterval("zr_sounds_moan_interval", FCVAR_NONE, "How often in seconds should zombies moan", 30.0f, true, 0.0f, false, 0.0f);
CConVar<bool> g_cvarNapalmGrenades("zr_napalm_enable", FCVAR_NONE, "Whether to use napalm grenades", true);
CConVar<float> g_cvarNapalmDuration("zr_napalm_burn_duration", FCVAR_NONE, "How long in seconds should zombies burn from napalm grenades", 5.0f, true, 0.0f, false, 0.0f);
CConVar<float> g_cvarNapalmFullDamage("zr_napalm_full_damage", FCVAR_NONE, "The amount of damage needed to apply full burn duration for napalm grenades (max grenade damage is 99)", 50.0f, true, 0.0f, true, 99.0f);
CConVar<CUtlString> g_cvarHumanWinOverlayParticle("zr_human_win_overlay_particle", FCVAR_NONE, "Screenspace particle to display when human win", "");
CConVar<CUtlString> g_cvarHumanWinOverlayMaterial("zr_human_win_overlay_material", FCVAR_NONE, "Material override for human's win overlay particle", "");
CConVar<float> g_cvarHumanWinOverlaySize("zr_human_win_overlay_size", FCVAR_NONE, "Size of human's win overlay particle", 100.0f, true, 0.0f, true, 100.0f);
CConVar<CUtlString> g_cvarZombieWinOverlayParticle("zr_zombie_win_overlay_particle", FCVAR_NONE, "Screenspace particle to display when zombie win", "");
CConVar<CUtlString> g_cvarZombieWinOverlayMaterial("zr_zombie_win_overlay_material", FCVAR_NONE, "Material override for zombie's win overlay particle", "");
CConVar<float> g_cvarZombieWinOverlaySize("zr_zombie_win_overlay_size", FCVAR_NONE, "Size of zombie's win overlay particle", 100.0f, true, 0.0f, true, 100.0f);
CConVar<bool> g_cvarInfectShake("zr_infect_shake", FCVAR_NONE, "Whether to shake a player's view on infect", true);
CConVar<float> g_cvarInfectShakeAmplitude("zr_infect_shake_amp", FCVAR_NONE, "Amplitude of shaking effect", 15.0f, true, 0.0f, true, 16.0f);
CConVar<float> g_cvarInfectShakeFrequency("zr_infect_shake_frequency", FCVAR_NONE, "Frequency of shaking effect", 2.0f, true, 0.0f, false, 0.0f);
CConVar<float> g_cvarInfectShakeDuration("zr_infect_shake_duration", FCVAR_NONE, "Duration of shaking effect", 5.0f, true, 0.0f, false, 0.0f);

// meant only for offline config validation and can easily cause issues when used on live server
#ifdef _DEBUG
CON_COMMAND_F(zr_reload_classes, "- Reload ZR player classes", FCVAR_SPONLY | FCVAR_LINKED_CONCOMMAND)
{
	g_pZRPlayerClassManager->LoadPlayerClass();

	Message("Reloaded ZR player classes.\n");
}
#endif

void ZR_Precache(IEntityResourceManifest* pResourceManifest)
{
	g_pZRPlayerClassManager->LoadPlayerClass();
	g_pZRPlayerClassManager->PrecacheModels(pResourceManifest);

	pResourceManifest->AddResource(g_cvarHumanWinOverlayParticle.Get().String());
	pResourceManifest->AddResource(g_cvarZombieWinOverlayParticle.Get().String());

	pResourceManifest->AddResource(g_cvarHumanWinOverlayMaterial.Get().String());
	pResourceManifest->AddResource(g_cvarZombieWinOverlayMaterial.Get().String());

	pResourceManifest->AddResource("soundevents/soundevents_zr.vsndevts");
}

void ZR_CreateOverlay(const char* pszOverlayParticlePath, float flAlpha, float flRadius, float flLifeTime, Color clrTint, const char* pszMaterialOverride)
{
	CEnvParticleGlow* particle = CreateEntityByName<CEnvParticleGlow>("env_particle_glow");

	CEntityKeyValues* pKeyValues = new CEntityKeyValues();

	pKeyValues->SetString("effect_name", pszOverlayParticlePath);
	// these properties are mapped to control point position by the entity
	pKeyValues->SetFloat("alphascale", flAlpha);		// 17.x
	pKeyValues->SetFloat("scale", flRadius);			// 17.y
	pKeyValues->SetFloat("selfillumscale", flLifeTime); // 17.z
	pKeyValues->SetColor("colortint", clrTint);			// 16.xyz

	pKeyValues->SetString("effect_textureOverride", pszMaterialOverride);

	particle->DispatchSpawn(pKeyValues);
	particle->AcceptInput("Start");

	UTIL_AddEntityIOEvent(particle, "Kill", nullptr, nullptr, "", flLifeTime + 1.0);
}

ZRModelEntry::ZRModelEntry(std::shared_ptr<ZRModelEntry> modelEntry) :
	szModelPath(modelEntry->szModelPath),
	szColor(modelEntry->szColor)
{
	vecSkins.Purge();
	FOR_EACH_VEC(modelEntry->vecSkins, i)
	{
		vecSkins.AddToTail(modelEntry->vecSkins[i]);
	}
};

ZRModelEntry::ZRModelEntry(ordered_json jsonModelEntry) :
	szModelPath(jsonModelEntry.value("modelname", "")),
	szColor(jsonModelEntry.value("color", "255 255 255"))
{
	vecSkins.Purge();

	if (jsonModelEntry.contains("skins"))
	{
		if (jsonModelEntry["skins"].size() > 0) // single int or array of ints
			for (auto& [key, skinIndex] : jsonModelEntry["skins"].items())
				vecSkins.AddToTail(skinIndex);
		return;
	}
	vecSkins.AddToTail(0); // key missing, set default
};

// seperate parsing to adminsystem's ParseFlags as making class 'z' flagged would make it available to players with non-zero flag
uint64 ZRClass::ParseClassFlags(const char* pszFlags)
{
	uint64 flags = 0;
	size_t length = V_strlen(pszFlags);

	for (size_t i = 0; i < length; i++)
	{
		char c = tolower(pszFlags[i]);
		if (c < 'a' || c > 'z')
			continue;

		flags |= ((uint64)1 << (c - 'a'));
	}

	return flags;
}

// this constructor is only used to create base class, which is required to have all values and min. 1 valid model entry
ZRClass::ZRClass(ordered_json jsonKeys, std::string szClassname, int iTeam) :
	iTeam(iTeam),
	bEnabled(jsonKeys["enabled"].get<bool>()),
	szClassName(szClassname),
	iHealth(jsonKeys["health"].get<int>()),
	flScale(jsonKeys["scale"].get<float>()),
	flSpeed(jsonKeys["speed"].get<float>()),
	flGravity(jsonKeys["gravity"].get<float>()),
	iAdminFlag(ParseClassFlags(
		jsonKeys["admin_flag"].get<std::string>().c_str()))
{
	vecModels.Purge();

	for (auto& [key, jsonModelEntry] : jsonKeys["models"].items())
	{
		std::shared_ptr<ZRModelEntry> modelEntry = std::make_shared<ZRModelEntry>(jsonModelEntry);
		vecModels.AddToTail(modelEntry);
	}
};

void ZRClass::Override(ordered_json jsonKeys, std::string szClassname)
{
	szClassName = szClassname;
	if (jsonKeys.contains("enabled"))
		bEnabled = jsonKeys["enabled"].get<bool>();
	if (jsonKeys.contains("health"))
		iHealth = jsonKeys["health"].get<int>();
	if (jsonKeys.contains("scale"))
		flScale = jsonKeys["scale"].get<float>();
	if (jsonKeys.contains("speed"))
		flSpeed = jsonKeys["speed"].get<float>();
	if (jsonKeys.contains("gravity"))
		flGravity = jsonKeys["gravity"].get<float>();
	if (jsonKeys.contains("admin_flag"))
		iAdminFlag = ParseClassFlags(
			jsonKeys["admin_flag"].get<std::string>().c_str());

	// no models entry key or it's empty, use model entries of base class
	if (!jsonKeys.contains("models") || jsonKeys["models"].empty())
		return;

	// one model entry in base and overriding class, apply model entry keys if defined
	if (vecModels.Count() == 1 && jsonKeys["models"].size() == 1)
	{
		if (jsonKeys["models"][0].contains("modelname"))
			vecModels[0]->szModelPath = jsonKeys["models"][0]["modelname"];
		if (jsonKeys["models"][0].contains("color"))
			vecModels[0]->szColor = jsonKeys["models"][0]["color"];
		if (jsonKeys["models"][0].contains("skins") && jsonKeys["models"][0]["skins"].size() > 0)
		{
			vecModels[0]->vecSkins.Purge();

			for (auto& [key, skinIndex] : jsonKeys["models"][0]["skins"].items())
				vecModels[0]->vecSkins.AddToTail(skinIndex);
		}

		return;
	}

	// more than one model entry in either base or child class, either override all entries or none
	for (int i = jsonKeys["models"].size() - 1; i >= 0; i--)
	{
		if (jsonKeys["models"][i].size() < 3)
		{
			Warning("Model entry in child class %s has empty key(s), skipping\n", szClassname.c_str());
			jsonKeys["models"].erase(i);
		}
	}

	if (jsonKeys["models"].empty())
	{
		Warning("No valid model entries remaining in child class %s, using model entries of base class %s\n",
				szClassname.c_str(), jsonKeys["base"].get<std::string>().c_str());
		return;
	}

	vecModels.Purge();

	for (auto& [key, jsonModelEntry] : jsonKeys["models"].items())
	{
		std::shared_ptr<ZRModelEntry> modelEntry = std::make_shared<ZRModelEntry>(jsonModelEntry);
		vecModels.AddToTail(modelEntry);
	}
}

ZRHumanClass::ZRHumanClass(ordered_json jsonKeys, std::string szClassname) :
	ZRClass(jsonKeys, szClassname, CS_TEAM_CT){};

ZRZombieClass::ZRZombieClass(ordered_json jsonKeys, std::string szClassname) :
	ZRClass(jsonKeys, szClassname, CS_TEAM_T),
	iHealthRegenCount(jsonKeys.value("health_regen_count", 0)),
	flHealthRegenInterval(jsonKeys.value("health_regen_interval", 0)),
	flKnockback(jsonKeys.value("knockback", 1.0)){};

void ZRZombieClass::Override(ordered_json jsonKeys, std::string szClassname)
{
	ZRClass::Override(jsonKeys, szClassname);
	if (jsonKeys.contains("health_regen_count"))
		iHealthRegenCount = jsonKeys["health_regen_count"].get<int>();
	if (jsonKeys.contains("health_regen_interval"))
		flHealthRegenInterval = jsonKeys["health_regen_interval"].get<float>();
	if (jsonKeys.contains("knockback"))
		flKnockback = jsonKeys["knockback"].get<float>();
}

bool ZRClass::IsApplicableTo(CCSPlayerController* pController)
{
	if (!bEnabled) return false;
	if (!V_stricmp(szClassName.c_str(), "MotherZombie")) return false;
	ZEPlayer* pPlayer = pController->GetZEPlayer();
	if (!pPlayer) return false;
	if (!pPlayer->IsAdminFlagSet(iAdminFlag)) return false;
	return true;
}

void CZRPlayerClassManager::PrecacheModels(IEntityResourceManifest* pResourceManifest)
{
	FOR_EACH_MAP_FAST(m_ZombieClassMap, i)
	{
		FOR_EACH_VEC(m_ZombieClassMap[i]->vecModels, j)
		{
			pResourceManifest->AddResource(m_ZombieClassMap[i]->vecModels[j]->szModelPath.c_str());
		}
	}
	FOR_EACH_MAP_FAST(m_HumanClassMap, i)
	{
		FOR_EACH_VEC(m_HumanClassMap[i]->vecModels, j)
		{
			pResourceManifest->AddResource(m_HumanClassMap[i]->vecModels[j]->szModelPath.c_str());
		}
	}
}

void CZRPlayerClassManager::LoadPlayerClass()
{
	Message("Loading PlayerClass...\n");
	m_ZombieClassMap.Purge();
	m_HumanClassMap.Purge();
	m_vecZombieDefaultClass.Purge();
	m_vecHumanDefaultClass.Purge();

	const char* pszJsonPath = "addons/cs2fixes/configs/zr/playerclass.jsonc";
	char szPath[MAX_PATH];
	V_snprintf(szPath, sizeof(szPath), "%s%s%s", Plat_GetGameDirectory(), "/csgo/", pszJsonPath);
	std::ifstream jsoncFile(szPath);

	if (!jsoncFile.is_open())
	{
		Panic("Failed to open %s. Playerclasses not loaded\n", pszJsonPath);
		return;
	}

	// Less code than constantly traversing the full class vectors, temporary lifetime anyways
	std::set<std::string> setClassNames;
	ordered_json jsonPlayerClasses = ordered_json::parse(jsoncFile, nullptr, false, true);

	if (jsonPlayerClasses.is_discarded())
	{
		Panic("Failed parsing JSON from %s. Playerclasses not loaded\n", pszJsonPath);
		return;
	}

	for (auto& [szTeamName, jsonTeamClasses] : jsonPlayerClasses.items())
	{
		bool bHuman = szTeamName == "Human";
		if (bHuman)
			Message("Human Classes:\n");
		else
			Message("Zombie Classes:\n");

		for (auto& [szClassName, jsonClass] : jsonTeamClasses.items())
		{
			bool bEnabled = jsonClass.value("enabled", false);
			bool bTeamDefault = jsonClass.value("team_default", false);

			std::string szBase = jsonClass.value("base", "");

			bool bMissingKey = false;

			if (setClassNames.contains(szClassName))
			{
				Panic("A class named %s already exists!\n", szClassName.c_str());
				bMissingKey = true;
			}

			if (!jsonClass.contains("team_default"))
			{
				Panic("%s has unspecified key: team_default\n", szClassName.c_str());
				bMissingKey = true;
			}

			// check everything if no base class
			if (szBase.empty())
			{
				if (!jsonClass.contains("health"))
				{
					Panic("%s has unspecified key: health\n", szClassName.c_str());
					bMissingKey = true;
				}
				if (!jsonClass.contains("models"))
				{
					Panic("%s has unspecified key: models\n", szClassName.c_str());
					bMissingKey = true;
				}
				else if (jsonClass["models"].size() < 1)
				{
					Panic("%s has no model entries\n", szClassName.c_str());
					bMissingKey = true;
				}
				else
				{
					for (auto& [key, jsonModelEntry] : jsonClass["models"].items())
					{
						if (!jsonModelEntry.contains("modelname"))
						{
							Panic("%s has unspecified model entry key: modelname\n", szClassName.c_str());
							bMissingKey = true;
						}
					}
					// BASE CLASS BEHAVIOUR: if not present, skins defaults to [0] and color defaults to "255 255 255"
				}
				if (!jsonClass.contains("scale"))
				{
					Panic("%s has unspecified key: scale\n", szClassName.c_str());
					bMissingKey = true;
				}
				if (!jsonClass.contains("speed"))
				{
					Panic("%s has unspecified key: speed\n", szClassName.c_str());
					bMissingKey = true;
				}
				if (!jsonClass.contains("gravity"))
				{
					Panic("%s has unspecified key: gravity\n", szClassName.c_str());
					bMissingKey = true;
				}
				/*if (!jsonClass.contains("knockback"))
				{
					Warning("%s has unspecified key: knockback\n", szClassName.c_str());
					bMissingKey = true;
				}*/
				if (!jsonClass.contains("admin_flag"))
				{
					Panic("%s has unspecified key: admin_flag\n", szClassName.c_str());
					bMissingKey = true;
				}
			}
			if (bMissingKey)
				continue;

			if (bHuman)
			{
				std::shared_ptr<ZRHumanClass> pHumanClass;
				if (!szBase.empty())
				{
					std::shared_ptr<ZRHumanClass> pBaseHumanClass = GetHumanClass(szBase.c_str());
					if (pBaseHumanClass)
					{
						pHumanClass = std::make_shared<ZRHumanClass>(pBaseHumanClass);
						pHumanClass->Override(jsonClass, szClassName);
					}
					else
					{
						Panic("Could not find specified base \"%s\" for %s!!!\n", szBase.c_str(), szClassName.c_str());
						continue;
					}
				}
				else
					pHumanClass = std::make_shared<ZRHumanClass>(jsonClass, szClassName);

				m_HumanClassMap.Insert(hash_32_fnv1a_const(szClassName.c_str()), pHumanClass);

				if (bTeamDefault)
					m_vecHumanDefaultClass.AddToTail(pHumanClass);

				pHumanClass->PrintInfo();
			}
			else
			{
				std::shared_ptr<ZRZombieClass> pZombieClass;
				if (!szBase.empty())
				{
					std::shared_ptr<ZRZombieClass> pBaseZombieClass = GetZombieClass(szBase.c_str());
					if (pBaseZombieClass)
					{
						pZombieClass = std::make_shared<ZRZombieClass>(pBaseZombieClass);
						pZombieClass->Override(jsonClass, szClassName);
					}
					else
					{
						Panic("Could not find specified base \"%s\" for %s!!!\n", szBase.c_str(), szClassName.c_str());
						continue;
					}
				}
				else
					pZombieClass = std::make_shared<ZRZombieClass>(jsonClass, szClassName);

				m_ZombieClassMap.Insert(hash_32_fnv1a_const(szClassName.c_str()), pZombieClass);
				if (bTeamDefault)
					m_vecZombieDefaultClass.AddToTail(pZombieClass);

				pZombieClass->PrintInfo();
			}

			setClassNames.insert(szClassName);
		}
	}
}

template <typename Out>
void split(const std::string& s, char delim, Out result)
{
	std::istringstream iss(s);
	std::string item;
	while (std::getline(iss, item, delim))
		*result++ = item;
}

void CZRPlayerClassManager::ApplyBaseClass(std::shared_ptr<ZRClass> pClass, CCSPlayerPawn* pPawn)
{
	std::shared_ptr<ZRModelEntry> pModelEntry = pClass->GetRandomModelEntry();
	Color clrRender;
	V_StringToColor(pModelEntry->szColor.c_str(), clrRender);

	pPawn->m_iMaxHealth = pClass->iHealth;
	pPawn->m_iHealth = pClass->iHealth;
	pPawn->SetModel(pModelEntry->szModelPath.c_str());
	pPawn->m_clrRender = clrRender;
	pPawn->AcceptInput("Skin", pModelEntry->GetRandomSkin());
	pPawn->m_flGravityScale = pClass->flGravity;

	// I don't know why, I don't want to know why,
	// I shouldn't have to wonder why, but for whatever reason
	// this shit caused crashes on ROUND END or MAP CHANGE after the 26/04/2024 update
	// pPawn->m_flVelocityModifier = pClass->flSpeed;
	const auto pController = reinterpret_cast<CCSPlayerController*>(pPawn->GetController());
	if (const auto pPlayer = pController != nullptr ? pController->GetZEPlayer() : nullptr)
	{
		pPlayer->SetMaxSpeed(pClass->flSpeed);
		pPlayer->SetActiveZRClass(pClass);
		pPlayer->SetActiveZRModel(pModelEntry);
	}

	// This has to be done a bit later
	UTIL_AddEntityIOEvent(pPawn, "SetScale", nullptr, nullptr, pClass->flScale);
}

// only changes that should not (directly) affect gameplay
void CZRPlayerClassManager::ApplyBaseClassVisuals(std::shared_ptr<ZRClass> pClass, CCSPlayerPawn* pPawn)
{
	std::shared_ptr<ZRModelEntry> pModelEntry = pClass->GetRandomModelEntry();
	Color clrRender;
	V_StringToColor(pModelEntry->szColor.c_str(), clrRender);

	pPawn->SetModel(pModelEntry->szModelPath.c_str());
	pPawn->m_clrRender = clrRender;
	pPawn->AcceptInput("Skin", pModelEntry->GetRandomSkin());

	const auto pController = reinterpret_cast<CCSPlayerController*>(pPawn->GetController());
	if (const auto pPlayer = pController != nullptr ? pController->GetZEPlayer() : nullptr)
	{
		pPlayer->SetActiveZRClass(pClass);
		pPlayer->SetActiveZRModel(pModelEntry);
	}

	// This has to be done a bit later
	UTIL_AddEntityIOEvent(pPawn, "SetScale", nullptr, nullptr, pClass->flScale);
}

std::shared_ptr<ZRHumanClass> CZRPlayerClassManager::GetHumanClass(const char* pszClassName)
{
	uint16 index = m_HumanClassMap.Find(hash_32_fnv1a_const(pszClassName));
	if (!m_HumanClassMap.IsValidIndex(index))
		return nullptr;
	return m_HumanClassMap[index];
}

void CZRPlayerClassManager::ApplyHumanClass(std::shared_ptr<ZRHumanClass> pClass, CCSPlayerPawn* pPawn)
{
	ApplyBaseClass(pClass, pPawn);
	CCSPlayerController* pController = CCSPlayerController::FromPawn(pPawn);
	if (pController)
		CZRRegenTimer::StopRegen(pController);

	if (!g_cvarEnableLeader.Get() || !pController)
		return;

	ZEPlayer* pPlayer = g_playerManager->GetPlayer(pController->GetPlayerSlot());

	if (pPlayer && pPlayer->IsLeader())
	{
		CHandle<CCSPlayerPawn> hPawn = pPawn->GetHandle();

		new CTimer(0.02f, false, false, [hPawn]() {
			CCSPlayerPawn* pPawn = hPawn.Get();
			if (pPawn)
				Leader_ApplyLeaderVisuals(pPawn);
			return -1.0f;
		});
	}
}

void CZRPlayerClassManager::ApplyPreferredOrDefaultHumanClass(CCSPlayerPawn* pPawn)
{
	CCSPlayerController* pController = CCSPlayerController::FromPawn(pPawn);
	if (!pController) return;

	// Get the human class user preference, or default if no class is set
	int iSlot = pController->GetPlayerSlot();
	std::shared_ptr<ZRHumanClass> humanClass = nullptr;
	const char* sPreferredHumanClass = g_pUserPreferencesSystem->GetPreference(iSlot, HUMAN_CLASS_KEY_NAME);

	// If the preferred human class exists and can be applied, override the default
	uint16 index = m_HumanClassMap.Find(hash_32_fnv1a_const(sPreferredHumanClass));
	if (m_HumanClassMap.IsValidIndex(index) && m_HumanClassMap[index]->IsApplicableTo(pController))
	{
		humanClass = m_HumanClassMap[index];
	}
	else if (m_vecHumanDefaultClass.Count())
	{
		humanClass = m_vecHumanDefaultClass[rand() % m_vecHumanDefaultClass.Count()];
	}
	else if (!humanClass)
	{
		Warning("Missing default human class or valid preferences!\n");
		return;
	}

	ApplyHumanClass(humanClass, pPawn);
}

void CZRPlayerClassManager::ApplyPreferredOrDefaultHumanClassVisuals(CCSPlayerPawn* pPawn)
{
	CCSPlayerController* pController = CCSPlayerController::FromPawn(pPawn);
	if (!pController) return;

	// Get the human class user preference, or default if no class is set
	int iSlot = pController->GetPlayerSlot();
	std::shared_ptr<ZRHumanClass> humanClass = nullptr;
	const char* sPreferredHumanClass = g_pUserPreferencesSystem->GetPreference(iSlot, HUMAN_CLASS_KEY_NAME);

	// If the preferred human class exists and can be applied, override the default
	uint16 index = m_HumanClassMap.Find(hash_32_fnv1a_const(sPreferredHumanClass));
	if (m_HumanClassMap.IsValidIndex(index) && m_HumanClassMap[index]->IsApplicableTo(pController))
	{
		humanClass = m_HumanClassMap[index];
	}
	else if (m_vecHumanDefaultClass.Count())
	{
		humanClass = m_vecHumanDefaultClass[rand() % m_vecHumanDefaultClass.Count()];
	}
	else if (!humanClass)
	{
		Warning("Missing default human class or valid preferences!\n");
		return;
	}

	ApplyBaseClassVisuals(humanClass, pPawn);
}

std::shared_ptr<ZRZombieClass> CZRPlayerClassManager::GetZombieClass(const char* pszClassName)
{
	uint16 index = m_ZombieClassMap.Find(hash_32_fnv1a_const(pszClassName));
	if (!m_ZombieClassMap.IsValidIndex(index))
		return nullptr;
	return m_ZombieClassMap[index];
}

void CZRPlayerClassManager::ApplyZombieClass(std::shared_ptr<ZRZombieClass> pClass, CCSPlayerPawn* pPawn)
{
	ApplyBaseClass(pClass, pPawn);
	CCSPlayerController* pController = CCSPlayerController::FromPawn(pPawn);
	if (pController)
		CZRRegenTimer::StartRegen(pClass->flHealthRegenInterval, pClass->iHealthRegenCount, pController);
}

void CZRPlayerClassManager::ApplyPreferredOrDefaultZombieClass(CCSPlayerPawn* pPawn)
{
	CCSPlayerController* pController = CCSPlayerController::FromPawn(pPawn);
	if (!pController) return;

	// Get the zombie class user preference, or default if no class is set
	int iSlot = pController->GetPlayerSlot();
	std::shared_ptr<ZRZombieClass> zombieClass = nullptr;
	const char* sPreferredZombieClass = g_pUserPreferencesSystem->GetPreference(iSlot, ZOMBIE_CLASS_KEY_NAME);

	// If the preferred zombie class exists and can be applied, override the default
	uint16 index = m_ZombieClassMap.Find(hash_32_fnv1a_const(sPreferredZombieClass));
	if (m_ZombieClassMap.IsValidIndex(index) && m_ZombieClassMap[index]->IsApplicableTo(pController))
	{
		zombieClass = m_ZombieClassMap[index];
	}
	else if (m_vecZombieDefaultClass.Count())
	{
		zombieClass = m_vecZombieDefaultClass[rand() % m_vecZombieDefaultClass.Count()];
	}
	else if (!zombieClass)
	{
		Warning("Missing default zombie class or valid preferences!\n");
		return;
	}

	ApplyZombieClass(zombieClass, pPawn);
}

void CZRPlayerClassManager::GetZRClassList(int iTeam, CUtlVector<std::shared_ptr<ZRClass>>& vecClasses, CCSPlayerController* pController)
{
	if (iTeam == CS_TEAM_T || iTeam == CS_TEAM_NONE)
	{
		FOR_EACH_MAP_FAST(m_ZombieClassMap, i)
		{
			if (!pController || m_ZombieClassMap[i]->IsApplicableTo(pController))
				vecClasses.AddToTail(m_ZombieClassMap[i]);
		}
	}

	if (iTeam == CS_TEAM_CT || iTeam == CS_TEAM_NONE)
	{
		FOR_EACH_MAP_FAST(m_HumanClassMap, i)
		{
			if (!pController || m_HumanClassMap[i]->IsApplicableTo(pController))
				vecClasses.AddToTail(m_HumanClassMap[i]);
		}
	}
}

double CZRRegenTimer::s_flNextExecution;
CZRRegenTimer* CZRRegenTimer::s_vecRegenTimers[MAXPLAYERS];

bool CZRRegenTimer::Execute()
{
	CCSPlayerPawn* pPawn = m_hPawnHandle.Get();
	if (!pPawn || !pPawn->IsAlive())
		return false;

	// Do we even need to regen?
	if (pPawn->m_iHealth() >= pPawn->m_iMaxHealth())
		return true;

	int iHealth = pPawn->m_iHealth() + m_iRegenAmount;
	pPawn->m_iHealth = pPawn->m_iMaxHealth() < iHealth ? pPawn->m_iMaxHealth() : iHealth;
	return true;
}

void CZRRegenTimer::StartRegen(float flRegenInterval, int iRegenAmount, CCSPlayerController* pController)
{
	int slot = pController->GetPlayerSlot();
	CZRRegenTimer* pTimer = s_vecRegenTimers[slot];
	if (pTimer != nullptr)
	{
		pTimer->m_flInterval = flRegenInterval;
		pTimer->m_iRegenAmount = iRegenAmount;
		return;
	}
	s_vecRegenTimers[slot] = new CZRRegenTimer(flRegenInterval, iRegenAmount, pController->m_hPlayerPawn());
}

void CZRRegenTimer::StopRegen(CCSPlayerController* pController)
{
	int slot = pController->GetPlayerSlot();
	if (!s_vecRegenTimers[slot])
		return;

	delete s_vecRegenTimers[slot];
	s_vecRegenTimers[slot] = nullptr;
}

void CZRRegenTimer::Tick()
{
	// check every timer every 0.1
	if (s_flNextExecution > g_flUniversalTime)
		return;

	VPROF("CZRRegenTimer::Tick");

	s_flNextExecution = g_flUniversalTime + 0.1f;
	for (int i = MAXPLAYERS - 1; i >= 0; i--)
	{
		CZRRegenTimer* pTimer = s_vecRegenTimers[i];
		if (!pTimer)
			continue;

		if (pTimer->m_flLastExecute == -1)
			pTimer->m_flLastExecute = g_flUniversalTime;

		// Timer execute
		if (pTimer->m_flLastExecute + pTimer->m_flInterval <= g_flUniversalTime)
		{
			pTimer->Execute();
			pTimer->m_flLastExecute = g_flUniversalTime;
		}
	}
}

void CZRRegenTimer::RemoveAllTimers()
{
	for (int i = MAXPLAYERS - 1; i >= 0; i--)
	{
		if (!s_vecRegenTimers[i])
			continue;
		delete s_vecRegenTimers[i];
		s_vecRegenTimers[i] = nullptr;
	}
}

void ZR_OnLevelInit()
{
	g_ZRRoundState = EZRRoundState::ROUND_START;

	// Delay one tick to override any .cfg's
	new CTimer(0.02f, false, true, []() {
		// Here we force some cvars that are necessary for the gamemode
		g_pEngineServer2->ServerCommand("mp_give_player_c4 0");
		g_pEngineServer2->ServerCommand("mp_friendlyfire 0");
		g_pEngineServer2->ServerCommand("mp_ignore_round_win_conditions 1");
		// Necessary to fix bots kicked/joining infinitely when forced to CT https://github.com/Source2ZE/ZombieReborn/issues/64
		g_pEngineServer2->ServerCommand("bot_quota_mode fill");
		g_pEngineServer2->ServerCommand("mp_autoteambalance 0");
		// These disable most of the buy menu for zombies
		g_pEngineServer2->ServerCommand("mp_weapons_allow_pistols 3");
		g_pEngineServer2->ServerCommand("mp_weapons_allow_smgs 3");
		g_pEngineServer2->ServerCommand("mp_weapons_allow_heavy 3");
		g_pEngineServer2->ServerCommand("mp_weapons_allow_rifles 3");

		return -1.0f;
	});

	g_pZRWeaponConfig->LoadWeaponConfig();
	g_pZRHitgroupConfig->LoadHitgroupConfig();
	SetupCTeams();
}

void ZRWeaponConfig::LoadWeaponConfig()
{
	m_WeaponMap.Purge();
	KeyValues* pKV = new KeyValues("Weapons");
	KeyValues::AutoDelete autoDelete(pKV);

	const char* pszPath = "addons/cs2fixes/configs/zr/weapons.cfg";

	if (!pKV->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		Warning("Failed to load %s\n", pszPath);
		return;
	}
	for (KeyValues* pKey = pKV->GetFirstSubKey(); pKey; pKey = pKey->GetNextKey())
	{
		const char* pszWeaponName = pKey->GetName();
		bool bEnabled = pKey->GetBool("enabled", false);
		float flKnockback = pKey->GetFloat("knockback", 1.0f);
		Message("%s knockback: %f\n", pszWeaponName, flKnockback);
		std::shared_ptr<ZRWeapon> weapon = std::make_shared<ZRWeapon>();
		if (!bEnabled)
			continue;

		weapon->flKnockback = flKnockback;

		m_WeaponMap.Insert(hash_32_fnv1a_const(pszWeaponName), weapon);
	}

	return;
}

std::shared_ptr<ZRWeapon> ZRWeaponConfig::FindWeapon(const char* pszWeaponName)
{
	uint16 index = m_WeaponMap.Find(hash_32_fnv1a_const(pszWeaponName));
	if (m_WeaponMap.IsValidIndex(index))
		return m_WeaponMap[index];

	return nullptr;
}

void ZRHitgroupConfig::LoadHitgroupConfig()
{
	m_HitgroupMap.Purge();
	KeyValues* pKV = new KeyValues("Hitgroups");
	KeyValues::AutoDelete autoDelete(pKV);

	const char* pszPath = "addons/cs2fixes/configs/zr/hitgroups.cfg";

	if (!pKV->LoadFromFile(g_pFullFileSystem, pszPath))
	{
		Warning("Failed to load %s\n", pszPath);
		return;
	}
	for (KeyValues* pKey = pKV->GetFirstSubKey(); pKey; pKey = pKey->GetNextKey())
	{
		const char* pszHitgroupName = pKey->GetName();
		float flKnockback = pKey->GetFloat("knockback", 1.0f);
		int iIndex = -1;

		if (!V_strcasecmp(pszHitgroupName, "Generic"))
			iIndex = 0;
		else if (!V_strcasecmp(pszHitgroupName, "Head"))
			iIndex = 1;
		else if (!V_strcasecmp(pszHitgroupName, "Chest"))
			iIndex = 2;
		else if (!V_strcasecmp(pszHitgroupName, "Stomach"))
			iIndex = 3;
		else if (!V_strcasecmp(pszHitgroupName, "LeftArm"))
			iIndex = 4;
		else if (!V_strcasecmp(pszHitgroupName, "RightArm"))
			iIndex = 5;
		else if (!V_strcasecmp(pszHitgroupName, "LeftLeg"))
			iIndex = 6;
		else if (!V_strcasecmp(pszHitgroupName, "RightLeg"))
			iIndex = 7;
		else if (!V_strcasecmp(pszHitgroupName, "Neck"))
			iIndex = 8;
		else if (!V_strcasecmp(pszHitgroupName, "Gear"))
			iIndex = 10;

		if (iIndex == -1)
		{
			Panic("Failed to load hitgroup %s, invalid name!", pszHitgroupName);
			continue;
		}

		std::shared_ptr<ZRHitgroup> hitGroup = std::make_shared<ZRHitgroup>();

		hitGroup->flKnockback = flKnockback;
		m_HitgroupMap.Insert(iIndex, hitGroup);
		Message("Loaded hitgroup %s at index %d with %f knockback\n", pszHitgroupName, iIndex, hitGroup->flKnockback);
	}

	return;
}

std::shared_ptr<ZRHitgroup> ZRHitgroupConfig::FindHitgroupIndex(int iIndex)
{
	uint16 index = m_HitgroupMap.Find(iIndex);
	// Message("We are finding hitgroup index with index: %d and index is: %d\n", iIndex, index);

	if (m_HitgroupMap.IsValidIndex(index))
	{
		// Message("We found valid index with (m_HitgroupMap[index]): %d\n", m_HitgroupMap[index]);
		return m_HitgroupMap[index];
	}

	return nullptr;
}

void ZR_RespawnAll()
{
	if (!GetGlobals())
		return;

	for (int i = 0; i < GetGlobals()->maxClients; i++)
	{
		CCSPlayerController* pController = CCSPlayerController::FromSlot(i);

		if (!pController || pController->m_bIsHLTV || (pController->m_iTeamNum() != CS_TEAM_CT && pController->m_iTeamNum() != CS_TEAM_T))
			continue;
		pController->Respawn();
	}
}

void ToggleRespawn(bool force = false, bool value = false)
{
	if ((!force && !g_bRespawnEnabled) || (force && value))
	{
		g_bRespawnEnabled = true;
		ZR_RespawnAll();
	}
	else
	{
		g_bRespawnEnabled = false;
		ZR_CheckTeamWinConditions(CS_TEAM_CT);
	}
}

void ZR_OnRoundPrestart(IGameEvent* pEvent)
{
	g_ZRRoundState = EZRRoundState::ROUND_START;
	ToggleRespawn(true, true);

	if (!GetGlobals())
		return;

	for (int i = 0; i < GetGlobals()->maxClients; i++)
	{
		CCSPlayerController* pController = CCSPlayerController::FromSlot(i);

		if (!pController || pController->m_bIsHLTV)
			continue;

		// Only do this for Ts, ignore CTs and specs
		if (pController->m_iTeamNum() == CS_TEAM_T)
			pController->SwitchTeam(CS_TEAM_CT);

		CCSPlayerPawn* pPawn = pController->GetPlayerPawn();

		// Prevent damage that occurs between now and when the round restart is finished
		// Somehow CT filtered nukes can apply damage during the round restart (all within CCSGameRules::RestartRound)
		// And if everyone was a zombie at this moment, they will all die and trigger ANOTHER round restart which breaks everything
		if (pPawn)
			pPawn->m_bTakesDamage = false;
	}
}

void SetupRespawnToggler()
{
	CBaseEntity* relay = CreateEntityByName("logic_relay");
	CEntityKeyValues* pKeyValues = new CEntityKeyValues();

	pKeyValues->SetString("targetname", "zr_toggle_respawn");
	relay->DispatchSpawn(pKeyValues);
	g_hRespawnToggler = relay->GetHandle();
}

void SetupCTeams()
{
	CTeam* pTeam = nullptr;
	while (nullptr != (pTeam = (CTeam*)UTIL_FindEntityByClassname(pTeam, "cs_team_manager")))
		if (pTeam->m_iTeamNum() == CS_TEAM_CT)
			g_hTeamCT = pTeam->GetHandle();
		else if (pTeam->m_iTeamNum() == CS_TEAM_T)
			g_hTeamT = pTeam->GetHandle();
}

void ZR_OnRoundStart(IGameEvent* pEvent)
{
	ClientPrintAll(HUD_PRINTTALK, ZR_PREFIX "The game is \x05Humans vs. Zombies\x01, the goal for zombies is to infect all humans by knifing them.");
	SetupRespawnToggler();
	CZRRegenTimer::RemoveAllTimers();

	if (!GetGlobals())
		return;

	for (int i = 0; i < GetGlobals()->maxClients; i++)
	{
		CCSPlayerController* pController = CCSPlayerController::FromSlot(i);

		if (!pController)
			continue;

		CCSPlayerPawn* pPawn = pController->GetPlayerPawn();

		// Now we can enable damage back
		if (pPawn)
			pPawn->m_bTakesDamage = true;
	}
}

void ZR_OnPlayerSpawn(CCSPlayerController* pController)
{
	// delay infection a bit
	bool bInfect = g_ZRRoundState == EZRRoundState::POST_INFECTION;

	// We're infecting this guy with a delay, disable all damage as they have 100 hp until then
	// also set team immediately in case the spawn teleport is team filtered
	if (bInfect)
	{
		pController->GetPawn()->m_bTakesDamage(false);
		pController->SwitchTeam(CS_TEAM_T);
	}
	else
	{
		pController->SwitchTeam(CS_TEAM_CT);
	}

	CHandle<CCSPlayerController> handle = pController->GetHandle();
	new CTimer(0.05f, false, false, [handle, bInfect]() {
		CCSPlayerController* pController = (CCSPlayerController*)handle.Get();
		if (!pController)
			return -1.0f;
		if (bInfect)
			ZR_Infect(pController, pController, true);
		else
			ZR_Cure(pController);
		return -1.0f;
	});
}

void ZR_ApplyKnockback(CCSPlayerPawn* pHuman, CCSPlayerPawn* pVictim, int iDamage, const char* szWeapon, int hitgroup, float classknockback)
{
	std::shared_ptr<ZRWeapon> pWeapon = g_pZRWeaponConfig->FindWeapon(szWeapon);
	std::shared_ptr<ZRHitgroup> pHitgroup = g_pZRHitgroupConfig->FindHitgroupIndex(hitgroup);
	// player shouldn't be able to pick up that weapon in the first place, but just in case
	if (!pWeapon)
		return;
	float flWeaponKnockbackScale = pWeapon->flKnockback;
	float flHitgroupKnockbackScale = 1.0f;

	if (pHitgroup)
		flHitgroupKnockbackScale = pHitgroup->flKnockback;

	Vector vecKnockback;
	AngleVectors(pHuman->m_angEyeAngles(), &vecKnockback);
	vecKnockback *= (iDamage * g_cvarKnockbackScale.Get() * flWeaponKnockbackScale * flHitgroupKnockbackScale * classknockback);
	pVictim->m_vecAbsVelocity = pVictim->m_vecAbsVelocity() + vecKnockback;
}

void ZR_ApplyKnockbackExplosion(CBaseEntity* pProjectile, CCSPlayerPawn* pVictim, int iDamage, bool bMolotov)
{
	std::shared_ptr<ZRWeapon> pWeapon = g_pZRWeaponConfig->FindWeapon(pProjectile->GetClassname());
	if (!pWeapon)
		return;
	float flWeaponKnockbackScale = pWeapon->flKnockback;

	Vector vecDisplacement = pVictim->GetAbsOrigin() - pProjectile->GetAbsOrigin();
	vecDisplacement.z += 36;
	VectorNormalize(vecDisplacement);
	Vector vecKnockback = vecDisplacement;

	if (bMolotov)
		vecKnockback.z = 0;

	vecKnockback *= (iDamage * g_cvarKnockbackScale.Get() * flWeaponKnockbackScale);
	pVictim->m_vecAbsVelocity = pVictim->m_vecAbsVelocity() + vecKnockback;
}

void ZR_FakePlayerDeath(CCSPlayerController* pAttackerController, CCSPlayerController* pVictimController, const char* szWeapon, bool bDontBroadcast)
{
	if (!pVictimController->m_bPawnIsAlive())
		return;

	IGameEvent* pEvent = g_gameEventManager->CreateEvent("player_death");

	if (!pEvent)
		return;

	pEvent->SetPlayer("userid", pVictimController->GetPlayerSlot());
	pEvent->SetPlayer("attacker", pAttackerController->GetPlayerSlot());
	pEvent->SetInt("assister", 65535);
	pEvent->SetInt("assister_pawn", -1);
	pEvent->SetString("weapon", szWeapon);
	pEvent->SetBool("infected", true);

	g_gameEventManager->FireEvent(pEvent, bDontBroadcast);
}

void ZR_StripAndGiveKnife(CCSPlayerPawn* pPawn)
{
	CCSPlayer_ItemServices* pItemServices = pPawn->m_pItemServices();
	CCSPlayer_WeaponServices* pWeaponServices = pPawn->m_pWeaponServices();

	// it can sometimes be null when player joined on the very first round?
	if (!pItemServices || !pWeaponServices)
		return;

	pPawn->DropMapWeapons();
	pItemServices->StripPlayerWeapons(true);

	if (pPawn->m_iTeamNum == CS_TEAM_T)
	{
		pItemServices->GiveNamedItem("weapon_knife_t");
	}
	else if (pPawn->m_iTeamNum == CS_TEAM_CT)
	{
		pItemServices->GiveNamedItem("weapon_knife");

		ConVarRefAbstract mp_free_armor("mp_free_armor");
		if (mp_free_armor.GetBool())
			pItemServices->GiveNamedItem("item_kevlar");
	}

	CUtlVector<CHandle<CBasePlayerWeapon>>* weapons = pWeaponServices->m_hMyWeapons();

	FOR_EACH_VEC(*weapons, i)
	{
		CBasePlayerWeapon* pWeapon = (*weapons)[i].Get();

		if (pWeapon && pWeapon->GetWeaponVData()->m_GearSlot() == GEAR_SLOT_KNIFE)
		{
			// Normally this isn't necessary, but there's a small window if infected right after throwing a grenade where this is needed
			pWeaponServices->SelectItem(pWeapon);
			break;
		}
	}
}

void ZR_Cure(CCSPlayerController* pTargetController)
{
	if (pTargetController->m_iTeamNum() == CS_TEAM_T)
		pTargetController->SwitchTeam(CS_TEAM_CT);

	ZEPlayer* pZEPlayer = pTargetController->GetZEPlayer();

	if (pZEPlayer)
		pZEPlayer->SetInfectState(false);

	CCSPlayerPawn* pTargetPawn = (CCSPlayerPawn*)pTargetController->GetPawn();
	if (!pTargetPawn)
		return;

	g_pZRPlayerClassManager->ApplyPreferredOrDefaultHumanClass(pTargetPawn);
}

float ZR_MoanTimer(ZEPlayerHandle hPlayer)
{
	if (!hPlayer.IsValid())
		return -1.f;

	if (!hPlayer.Get()->IsInfected())
		return -1.f;

	CCSPlayerPawn* pPawn = CCSPlayerController::FromSlot(hPlayer.GetPlayerSlot())->GetPlayerPawn();

	if (!pPawn || pPawn->m_iTeamNum == CS_TEAM_CT)
		return -1.f;

	// This guy is dead but still infected, and corpses are quiet
	if (!pPawn->IsAlive())
		return g_cvarMoanInterval.Get() + (rand() % 5);

	pPawn->EmitSound("zr.amb.zombie_voice_idle");

	return g_cvarMoanInterval.Get() + (rand() % 5);
}

void ZR_InfectShake(CCSPlayerController* pController)
{
	if (!pController || !pController->IsConnected() || pController->IsBot() || !g_cvarInfectShake.Get())
		return;

	INetworkMessageInternal* pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("Shake");

	auto data = pNetMsg->AllocateMessage()->ToPB<CUserMessageShake>();

	data->set_duration(g_cvarInfectShakeDuration.Get());
	data->set_frequency(g_cvarInfectShakeFrequency.Get());
	data->set_amplitude(g_cvarInfectShakeAmplitude.Get());
	data->set_command(0);

	CSingleRecipientFilter filter(pController->GetPlayerSlot());
	g_gameEventSystem->PostEventAbstract(-1, false, &filter, pNetMsg, data, 0);

	delete data;
}

std::vector<SpawnPoint*> ZR_GetSpawns()
{
	std::vector<SpawnPoint*> spawns;

	if (!g_pGameRules)
		return spawns;

	CUtlVector<SpawnPoint*>* ctSpawns = g_pGameRules->m_CTSpawnPoints();
	CUtlVector<SpawnPoint*>* tSpawns = g_pGameRules->m_TerroristSpawnPoints();

	FOR_EACH_VEC(*ctSpawns, i)
	spawns.push_back((*ctSpawns)[i]);

	FOR_EACH_VEC(*tSpawns, i)
	spawns.push_back((*tSpawns)[i]);

	if (!spawns.size())
		Panic("There are no spawns!\n");

	return spawns;
}

void ZR_Infect(CCSPlayerController* pAttackerController, CCSPlayerController* pVictimController, bool bDontBroadcast)
{
	// 如果被感染玩家不存在或不在CT队伍(人类队伍),则不执行感染
	if (!pVictimController || pVictimController->m_iTeamNum() != CS_TEAM_CT)
		return;

	// 获取被感染玩家的Pawn实体
	CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pVictimController->GetPawn();
	if (!pPawn)
		return;

	// 如果玩家已经死亡,则不执行感染
	if (!pPawn->IsAlive())
		return;

	// 创建一个假的玩家死亡事件,不广播给客户端
	ZR_FakePlayerDeath(pAttackerController, pVictimController, pAttackerController ? "knife" : "", bDontBroadcast);

	// 将玩家切换到T队伍(僵尸队伍)
	pVictimController->SwitchTeam(CS_TEAM_CT);

	// 添加感染效果的屏幕震动
	ZR_InfectShake(pVictimController);

	// 设置超速状态(如果有此功能)
	ZEPlayer* pZEPlayer = pVictimController->GetZEPlayer();
	if (pZEPlayer)
	{
		pZEPlayer->SetSuperSpeed(false);
	}

	// 检查CT队伍是否还有存活玩家,如果没有,则判定T队伍(僵尸)获胜
	if (!ZR_CheckTeamWinConditions(CS_TEAM_T))
	{
		// 创建一个延迟1帧的计时器,使用僵尸类的设置来重生玩家
		CHandle<CCSPlayerController> handle = pVictimController->GetHandle();
		new CTimer(0.1f, false, false, [handle]() {
			CCSPlayerController* pController = (CCSPlayerController*)handle.Get();
			if (!pController)
				return -1.0f;

			CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pController->GetPawn();
			if (!pPawn)
				return -1.0f;

			// 应用默认或用户首选的僵尸类别
			g_pZRPlayerClassManager->ApplyPreferredOrDefaultZombieClass(pPawn);

			// 如果未启用重生功能,则检查CT队伍(僵尸)是否获胜
			if (!g_bRespawnEnabled)
				ZR_CheckTeamWinConditions(CS_TEAM_T);

			return -1.0f;
		});
	}
}

void ZR_InfectMotherZombie(CCSPlayerController* pVictimController, std::vector<SpawnPoint*> spawns)
{
	// 如果被选为母体僵尸的玩家不存在,则退出
	if (!pVictimController)
		return;

	// 获取玩家的Pawn实体
	CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pVictimController->GetPawn();
	if (!pPawn)
		return;

	// 如果玩家已经不是人类(CT),则退出
	if (pVictimController->m_iTeamNum() != CS_TEAM_CT)
		return;

	// 根据设置的母体僵尸生成类型进行处理
	if (g_cvarInfectSpawnType.Get() == (int)EZRSpawnType::RESPAWN && spawns.size() > 0)
	{
		// 获取一个随机的重生点
		SpawnPoint* pSpawnPoint = spawns[rand() % spawns.size()];

		// 重置玩家的速度,避免被传送后仍保持移动
		pPawn->m_vecAbsVelocity().x = 0.0f;
		pPawn->m_vecAbsVelocity().y = 0.0f;
		pPawn->m_vecAbsVelocity().z = 0.0f;

		// 将玩家传送到重生点
		pPawn->Teleport(&pSpawnPoint->GetOrigin(), &pSpawnPoint->GetAngles(), nullptr);
	}

	// 对玩家执行感染处理,并广播感染消息
	ZR_Infect(nullptr, pVictimController, false);

	// 向所有玩家发送消息,告知谁是母体僵尸
	ClientPrintAll(HUD_PRINTTALK, ZR_PREFIX "%s 已被选为母体僵尸!", pVictimController->GetPlayerName());
}

// make players who've been picked as MZ recently less likely to be picked again
// store a variable in ZEPlayer, which gets initialized with value 100 if they are picked to be a mother zombie
// the value represents a % chance of the player being skipped next time they are picked to be a mother zombie
// If the player is skipped, next random player is picked to be mother zombie (and same skip chance logic applies to him)
// the variable gets decreased by 20 every round
void ZR_InitialInfection()
{
    // 检查全局变量是否可用
    if (!GetGlobals())
        return;

    // 创建一个向量存储所有可以被选为母体僵尸的玩家控制器
    CUtlVector<CCSPlayerController*> pCandidateControllers;
    for (int i = 0; i < GetGlobals()->maxClients; i++)
    {
        // 获取玩家控制器
        CCSPlayerController* pController = CCSPlayerController::FromSlot(i);
        // 检查玩家是否已连接且在CT阵营(人类阵营)
        if (!pController || !pController->IsConnected() || pController->m_iTeamNum() != CS_TEAM_CT)
            continue;

        // 获取玩家角色模型并检查是否存活
        CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pController->GetPawn();
        if (!pPawn || !pPawn->IsAlive())
            continue;

        // 将符合条件的玩家添加到候选列表
        pCandidateControllers.AddToTail(pController);
    }

    // 检查母体僵尸比例设置是否有效
    if (g_cvarInfectSpawnMZRatio.Get() <= 0)
    {
        Warning("母体僵尸比例设置无效!!!");
        return;
    }

    // 计算需要感染的母体僵尸数量
    // 根据玩家总数除以设定的比例,确保至少有最小数量的母体僵尸
    int iMZToInfect = pCandidateControllers.Count() / g_cvarInfectSpawnMZRatio.Get();
    iMZToInfect = g_cvarInfectSpawnMinCount.Get() > iMZToInfect ? g_cvarInfectSpawnMinCount.Get() : iMZToInfect;
    bool vecIsMZ[MAXPLAYERS] = {false}; // 记录哪些玩家已经被选为母体僵尸

    // 获取重生点位置
    std::vector<SpawnPoint*> spawns = ZR_GetSpawns();
    // 如果重生类型设置为RESPAWN但没有重生点,则发出警告并退出
    if (g_cvarInfectSpawnType.Get() == (int)EZRSpawnType::RESPAWN && !spawns.size())
    {
        ClientPrintAll(HUD_PRINTTALK, ZR_PREFIX "地图上没有重生点!");
        return;
    }

    // 开始感染过程
    int iFailSafeCounter = 0; // 防止无限循环的计数器
    while (iMZToInfect > 0)
    {
        // 故障保护机制:如果循环5次以上仍未选出足够的母体僵尸
        if (iFailSafeCounter >= 5)
        {
            FOR_EACH_VEC(pCandidateControllers, i)
            {
                // 在第5次循环:重置所有非母体僵尸的免疫力
                // 在第6次循环:重置所有玩家的免疫力(除了本轮已选的母体僵尸)
                ZEPlayer* pPlayer = pCandidateControllers[i]->GetZEPlayer();
                if (pPlayer->GetImmunity() < 100 || (iFailSafeCounter >= 6 && !vecIsMZ[i]))
                    pPlayer->SetImmunity(0);
            }
        }

        // 创建一个列表,存储上一轮母体僵尸选择中幸存的玩家
        CUtlVector<CCSPlayerController*> pSurvivorControllers;
        FOR_EACH_VEC(pCandidateControllers, i)
        {
            // 跳过已经被选为母体僵尸或拥有100%免疫力的玩家
            ZEPlayer* pPlayer = pCandidateControllers[i]->GetZEPlayer();
            if (pPlayer && pPlayer->GetImmunity() < 100)
                pSurvivorControllers.AddToTail(pCandidateControllers[i]);
        }

        // 如果触发最后的故障保护后仍没有可选的人类,则退出循环
        if (iFailSafeCounter >= 6 && pSurvivorControllers.Count() == 0)
            break;

        // 随机选择玩家作为母体僵尸
        while (pSurvivorControllers.Count() > 0 && iMZToInfect > 0)
        {
            // 随机选择一个玩家索引
            int randomindex = rand() % pSurvivorControllers.Count();

            CCSPlayerController* pController = (CCSPlayerController*)pSurvivorControllers[randomindex];
            CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pController->GetPawn();
            ZEPlayer* pPlayer = pSurvivorControllers[randomindex]->GetZEPlayer();
            
            // 检查玩家免疫力,如果随机数小于玩家免疫力,则跳过该玩家
            if (rand() % 100 < pPlayer->GetImmunity())
            {
                pSurvivorControllers.FastRemove(randomindex);
                continue;
            }

            // 将玩家感染为母体僵尸
            ZR_InfectMotherZombie(pController, spawns);
            pPlayer->SetImmunity(100); // 设置100%免疫力,防止再次被选中
            vecIsMZ[pPlayer->GetPlayerSlot().Get()] = true; // 标记为母体僵尸

            iMZToInfect--; // 减少需要感染的母体僵尸数量
        }
        iFailSafeCounter++; // 增加故障保护计数器
    }

    // 为所有非母体僵尸的玩家减少免疫力
    for (int i = 0; i < GetGlobals()->maxClients; i++)
    {
        ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);
        if (!pPlayer || vecIsMZ[i])
            continue;

        // 根据设置减少玩家的免疫力
        pPlayer->SetImmunity(pPlayer->GetImmunity() - g_cvarMZImmunityReduction.Get());
    }

    // 如果重生延迟设置为负值,则禁用重生功能
    if (g_cvarRespawnDelay.Get() < 0.0f)
        g_bRespawnEnabled = false;

    // 向所有玩家发送第一波感染已开始的消息
    ClientPrintAll(HUD_PRINTCENTER, "第一波感染已开始!");
    ClientPrintAll(HUD_PRINTTALK, ZR_PREFIX "第一波感染已开始! 祝你好运,幸存者们!");
    
    // 更新回合状态为感染后阶段
    g_ZRRoundState = EZRRoundState::POST_INFECTION;
}

// 创建并启动初始感染倒计时
void ZR_StartInitialCountdown()
{
	// 不允许多次触发初始感染
	if (g_ZRRoundState != EZRRoundState::ROUND_START)
		return;

	// 获取最小和最大感染时间范围
	int min = g_cvarInfectSpawnTimeMin.Get();
	int max = g_cvarInfectSpawnTimeMax.Get();

	// 如果最大时间小于最小时间,则交换它们
	if (max < min)
	{
		int t = min;
		min = max;
		max = t;
	}

	// 在最小和最大时间范围内随机选择一个感染时间点
	g_iInfectionCountDown = min == max ? min : min + (rand() % (max - min + 1));

	// 创建一个每秒触发一次的计时器
	new CTimer(1.0f, true, true, []() {
		// 如果回合已经结束,则停止倒计时
		if (g_ZRRoundState == EZRRoundState::ROUND_END)
			return -1.0f;

		// 计数器递减
		g_iInfectionCountDown--;
		
		// 根据倒计时剩余时间显示不同的提示消息
		if (g_iInfectionCountDown > 0)
		{
			// 在关键时间点显示倒计时警告(5秒、10秒、20秒等)
			if (g_iInfectionCountDown == 5 || g_iInfectionCountDown == 10 || g_iInfectionCountDown == 20 || g_iInfectionCountDown == 30 || g_iInfectionCountDown == 60)
			{
				ClientPrintAll(HUD_PRINTCENTER, "%d 秒后开始感染", g_iInfectionCountDown);
				ClientPrintAll(HUD_PRINTTALK, ZR_PREFIX "%d 秒后开始感染", g_iInfectionCountDown);
			}
			return 1.0f; // 继续倒计时
		}
		else
		{
			// 倒计时结束,开始执行初始感染
			ZR_InitialInfection();
			return -1.0f; // 停止计时器
		}
	});
}

bool ZR_Hook_OnTakeDamage_Alive(CTakeDamageInfo* pInfo, CCSPlayerPawn* pVictimPawn)
{
	CCSPlayerPawn* pAttackerPawn = (CCSPlayerPawn*)pInfo->m_hAttacker.Get();

	if (!(pAttackerPawn && pVictimPawn && pAttackerPawn->IsPawn() && pVictimPawn->IsPawn()))
		return false;

	CCSPlayerController* pAttackerController = CCSPlayerController::FromPawn(pAttackerPawn);
	CCSPlayerController* pVictimController = CCSPlayerController::FromPawn(pVictimPawn);
	const char* pszAbilityClass = pInfo->m_hAbility.Get() ? pInfo->m_hAbility.Get()->GetClassname() : "";
	
	// 在训练模式下禁用僵尸感染功能
	/*
	if (pAttackerPawn->m_iTeamNum() == CS_TEAM_T && pVictimPawn->m_iTeamNum() == CS_TEAM_CT && !V_strncmp(pszAbilityClass, "weapon_knife", 12))
	{
		ZR_Infect(pAttackerController, pVictimController, false);
		return true; // nullify the damage
	}
	*/

	if (g_cvarGroanChance.Get() && pVictimPawn->m_iTeamNum() == CS_TEAM_T && (rand() % g_cvarGroanChance.Get()) == 1)
		pVictimPawn->EmitSound("zr.amb.zombie_pain");

	// grenade and molotov knockback
	if (pAttackerPawn->m_iTeamNum() == CS_TEAM_CT && pVictimPawn->m_iTeamNum() == CS_TEAM_T)
	{
		CBaseEntity* pInflictor = pInfo->m_hInflictor.Get();
		const char* pszInflictorClass = pInflictor ? pInflictor->GetClassname() : "";
		// inflictor class from grenade damage is actually hegrenade_projectile
		bool bGrenade = V_strncmp(pszInflictorClass, "hegrenade", 9) == 0;
		bool bInferno = V_strncmp(pszInflictorClass, "inferno", 7) == 0;

		if (g_cvarNapalmGrenades.Get() && bGrenade)
		{
			// Scale burn duration by damage, so nades from farther away burn zombies for less time
			float flDuration = (pInfo->m_flDamage / g_cvarNapalmFullDamage.Get()) * g_cvarNapalmDuration.Get();
			flDuration = clamp(flDuration, 0.0f, g_cvarNapalmDuration.Get());

			// Can't use the same inflictor here as it'll end up calling this again each burn damage tick
			// DMG_BURN makes loud noises so use DMG_FALL instead which is completely silent
			IgnitePawn(pVictimPawn, flDuration, pAttackerPawn, pAttackerPawn, nullptr, DMG_FALL);
		}

		if (bGrenade || bInferno)
			ZR_ApplyKnockbackExplosion((CBaseEntity*)pInflictor, (CCSPlayerPawn*)pVictimPawn, (int)pInfo->m_flDamage, bInferno);
	}
	return false;
}

// return false to prevent player from picking it up
bool ZR_Detour_CCSPlayer_WeaponServices_CanUse(CCSPlayer_WeaponServices* pWeaponServices, CBasePlayerWeapon* pPlayerWeapon)
{
	CCSPlayerPawn* pPawn = pWeaponServices->__m_pChainEntity();
	if (!pPawn)
		return false;
	const char* pszWeaponClassname = pPlayerWeapon->GetWeaponClassname();
	if (pPawn->m_iTeamNum() == CS_TEAM_T && !CCSPlayer_ItemServices::IsAwsProcessing() && V_strncmp(pszWeaponClassname, "weapon_knife", 12) && V_strncmp(pszWeaponClassname, "weapon_c4", 9))
		return false;
	if (pPawn->m_iTeamNum() == CS_TEAM_CT && V_strlen(pszWeaponClassname) > 7 && !g_pZRWeaponConfig->FindWeapon(pszWeaponClassname + 7))
		return false;
	// doesn't guarantee the player will pick the weapon up, it just allows the original function to run
	return true;
}

void ZR_Detour_CEntityIdentity_AcceptInput(CEntityIdentity* pThis, CUtlSymbolLarge* pInputName, CEntityInstance* pActivator, CEntityInstance* pCaller, variant_t* value, int nOutputID)
{
	if (!g_hRespawnToggler.IsValid())
		return;

	CBaseEntity* relay = g_hRespawnToggler.Get();
	const char* inputName = pInputName->String();

	// Must be an input into our zr_toggle_respawn relay
	if (!relay || pThis != relay->m_pEntity)
		return;

	if (!V_strcasecmp(inputName, "Trigger"))
		ToggleRespawn();
	else if (!V_strcasecmp(inputName, "Enable") && !g_bRespawnEnabled)
		ToggleRespawn(true, true);
	else if (!V_strcasecmp(inputName, "Disable") && g_bRespawnEnabled)
		ToggleRespawn(true, false);
	else
		return;

	ClientPrintAll(HUD_PRINTTALK, ZR_PREFIX "Respawning is %s!", g_bRespawnEnabled ? "enabled" : "disabled");
}

void SpawnPlayer(CCSPlayerController* pController)
{
	// 在训练模式下,玩家始终加入CT阵营(人类阵营),忽略游戏当前阶段
	pController->ChangeTeam(CS_TEAM_CT);

	// 确保在玩家进入空服务器时结束回合
	if (!ZR_IsTeamAlive(CS_TEAM_CT) && !ZR_IsTeamAlive(CS_TEAM_T) && g_ZRRoundState != EZRRoundState::ROUND_END)
	{
		if (!g_pGameRules)
			return;

		// 结束当前回合,设置原因为游戏开始
		g_pGameRules->TerminateRound(1.0f, CSRoundEndReason::GameStart);
		g_ZRRoundState = EZRRoundState::ROUND_END;
		return;
	}

	// 创建一个计时器,2秒后重生玩家
	CHandle<CCSPlayerController> handle = pController->GetHandle();
	new CTimer(2.0f, false, false, [handle]() {
		CCSPlayerController* pController = (CCSPlayerController*)handle.Get();
		// 如果玩家无效,或重生功能已禁用,或玩家不在有效队伍中,则不重生
		if (!pController || !g_bRespawnEnabled || pController->m_iTeamNum < CS_TEAM_T)
			return -1.0f;
		pController->Respawn();
		return -1.0f; // 返回-1表示这是一次性计时器,不再重复执行
	});
}

void ZR_Hook_ClientPutInServer(CPlayerSlot slot, char const* pszName, int type, uint64 xuid)
{
	// 当玩家连接到服务器时触发此钩子
	CCSPlayerController* pController = CCSPlayerController::FromSlot(slot);
	if (!pController)
		return;

	// 调用SpawnPlayer函数使玩家加入游戏
	SpawnPlayer(pController);
}

void ZR_Hook_ClientCommand_JoinTeam(CPlayerSlot slot, const CCommand& args)
{
	// 当玩家执行加入队伍命令时触发此钩子
	CCSPlayerController* pController = CCSPlayerController::FromSlot(slot);
	if (!pController)
		return;

	// 如果玩家当前活着,先让其自杀
	CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pController->GetPawn();
	if (pPawn && pPawn->IsAlive())
		pPawn->CommitSuicide(false, true);

	// 解析参数:如果玩家要求加入观察者(参数为1),则切换到观察者队伍
	if (args.ArgC() >= 2 && !V_strcmp(args.Arg(1), "1"))
		pController->SwitchTeam(CS_TEAM_SPECTATOR);
	// 如果玩家当前是观察者,则调用SpawnPlayer使其加入游戏
	else if (pController->m_iTeamNum == CS_TEAM_SPECTATOR)
		SpawnPlayer(pController);
}

void ZR_OnPlayerHurt(IGameEvent* pEvent)
{
	// 当玩家受伤时触发此事件处理函数
	// 获取攻击者、受害者以及伤害相关的信息
	CCSPlayerController* pAttackerController = (CCSPlayerController*)pEvent->GetPlayerController("attacker");
	CCSPlayerController* pVictimController = (CCSPlayerController*)pEvent->GetPlayerController("userid");
	const char* szWeapon = pEvent->GetString("weapon");
	int iDmgHealth = pEvent->GetInt("dmg_health");
	int iHitGroup = pEvent->GetInt("hitgroup");

	// 手榴弹和燃烧弹的击退效果由TakeDamage拦截器处理,此处不处理
	if (!pAttackerController || !pVictimController || !V_strncmp(szWeapon, "inferno", 7) || !V_strncmp(szWeapon, "hegrenade", 9))
		return;

	// 如果是人类(CT)攻击僵尸(T),则应用击退效果
	if (pAttackerController->m_iTeamNum() == CS_TEAM_CT && pVictimController->m_iTeamNum() == CS_TEAM_T)
	{
		float flClassKnockback = 1.0f; // 默认击退系数为1.0

		// 获取僵尸的类别特定击退系数
		if (pVictimController->GetZEPlayer())
		{
			std::shared_ptr<ZRClass> activeClass = pVictimController->GetZEPlayer()->GetActiveZRClass();

			if (activeClass && activeClass->iTeam == CS_TEAM_T)
				flClassKnockback = static_pointer_cast<ZRZombieClass>(activeClass)->flKnockback;
		}

		// 应用击退效果
		ZR_ApplyKnockback((CCSPlayerPawn*)pAttackerController->GetPawn(), (CCSPlayerPawn*)pVictimController->GetPawn(), iDmgHealth, szWeapon, iHitGroup, flClassKnockback);
	}
}

void ZR_OnPlayerDeath(IGameEvent* pEvent)
{
	// 当玩家死亡时触发此事件处理函数
	
	// 如果是感染导致的假死亡事件,不需要重生或检查胜利条件
	if (pEvent->GetBool("infected"))
		return;

	// 获取死亡的玩家控制器和Pawn
	CCSPlayerController* pVictimController = (CCSPlayerController*)pEvent->GetPlayerController("userid");
	if (!pVictimController)
		return;
	CCSPlayerPawn* pVictimPawn = (CCSPlayerPawn*)pVictimController->GetPawn();
	if (!pVictimPawn)
		return;

	// 检查对方队伍是否已经获胜(全部消灭了对手)
	ZR_CheckTeamWinConditions(pVictimPawn->m_iTeamNum() == CS_TEAM_T ? CS_TEAM_CT : CS_TEAM_T);

	// 如果死亡的是僵尸,且在感染后阶段,播放僵尸死亡音效
	if (pVictimPawn->m_iTeamNum() == CS_TEAM_T && g_ZRRoundState == EZRRoundState::POST_INFECTION)
		pVictimPawn->EmitSound("zr.amb.zombie_die");

	// 设置定时器重生玩家
	CHandle<CCSPlayerController> handle = pVictimController->GetHandle();
	new CTimer(g_cvarRespawnDelay.Get() < 0.0f ? 2.0f : g_cvarRespawnDelay.Get(), false, false, [handle]() {
		CCSPlayerController* pController = (CCSPlayerController*)handle.Get();
		if (!pController || !g_bRespawnEnabled || pController->m_iTeamNum < CS_TEAM_T)
			return -1.0f;
		pController->Respawn();
		return -1.0f;
	});
}

void ZR_OnRoundFreezeEnd(IGameEvent* pEvent)
{
	// 在训练模式下禁用初始感染流程
	// ZR_StartInitialCountdown();
	
	// 设置回合状态为正常开始,不触发感染流程
	g_ZRRoundState = EZRRoundState::ROUND_START;
}

// 可能还有更好的方法来检查时间即将结束...
void ZR_OnRoundTimeWarning(IGameEvent* pEvent)
{
	// 当回合时间警告事件触发时执行此函数
	// 创建一个10秒后执行的计时器,以结束回合
	new CTimer(10.0, false, false, []() {
		if (g_ZRRoundState == EZRRoundState::ROUND_END)
			return -1.0f;
		// 根据配置的默认获胜队伍结束回合
		ZR_EndRoundAndAddTeamScore(g_cvarDefaultWinnerTeam.Get());
		return -1.0f;
	});
}

// 检查一个队伍是否有存活的玩家
bool ZR_IsTeamAlive(int iTeamNum)
{
	// 遍历所有玩家实体
	CCSPlayerPawn* pPawn = nullptr;
	while (nullptr != (pPawn = (CCSPlayerPawn*)UTIL_FindEntityByClassname(pPawn, "player")))
	{
		// 跳过已死亡的玩家
		if (!pPawn->IsAlive())
			continue;

		// 如果找到指定队伍的存活玩家,返回true
		if (pPawn->m_iTeamNum() == iTeamNum)
			return true;
	}
	// 没有找到指定队伍的存活玩家,返回false
	return false;
}

// 检查一个队伍是否获胜,如果是,结束回合并增加得分
bool ZR_CheckTeamWinConditions(int iTeamNum)
{
	// 如果回合已经结束,或者是CT队伍且重生功能已启用,或者是无效队伍,则返回false
	if (g_ZRRoundState == EZRRoundState::ROUND_END || (iTeamNum == CS_TEAM_CT && g_bRespawnEnabled) || (iTeamNum != CS_TEAM_T && iTeamNum != CS_TEAM_CT))
		return false;

	// 检查对方队伍是否还有存活玩家
	if (ZR_IsTeamAlive(iTeamNum == CS_TEAM_CT ? CS_TEAM_T : CS_TEAM_CT))
		return false;

	// 允许队伍获胜
	ZR_EndRoundAndAddTeamScore(iTeamNum);

	return true;
}

// 观察者:平局
// T:T胜利,增加T队分数
// CT:CT胜利,增加CT队分数
void ZR_EndRoundAndAddTeamScore(int iTeamNum)
{
	// 检查服务器是否空闲
	bool bServerIdle = true;

	if (!GetGlobals() || !g_pGameRules)
		return;

	// 遍历所有客户端,检查是否有真实玩家
	for (int i = 0; i < GetGlobals()->maxClients; i++)
	{
		ZEPlayer* pPlayer = g_playerManager->GetPlayer(i);

		if (!pPlayer || !pPlayer->IsConnected() || !pPlayer->IsInGame() || pPlayer->IsFakeClient())
			continue;

		bServerIdle = false;
		break;
	}

	// 服务器空闲时不结束回合
	if (bServerIdle)
		return;

	CSRoundEndReason iReason;
	switch (iTeamNum)
	{
		default:
		case CS_TEAM_SPECTATOR:
			iReason = CSRoundEndReason::Draw;
			break;
		case CS_TEAM_T:
			iReason = CSRoundEndReason::TerroristWin;
			break;
		case CS_TEAM_CT:
			iReason = CSRoundEndReason::CTWin;
			break;
	}

	static ConVarRefAbstract mp_round_restart_delay("mp_round_restart_delay");
	float flRestartDelay = mp_round_restart_delay.GetFloat();

	g_pGameRules->TerminateRound(flRestartDelay, iReason);
	g_ZRRoundState = EZRRoundState::ROUND_END;
	ToggleRespawn(true, false);

	if (iTeamNum == CS_TEAM_CT)
	{
		if (!g_hTeamCT.Get())
		{
			Panic("Cannot find CTeam for CT!\n");
			return;
		}
		g_hTeamCT->m_iScore = g_hTeamCT->m_iScore() + 1;
		if (g_cvarHumanWinOverlayParticle.Get().Length() != 0)
			ZR_CreateOverlay(g_cvarHumanWinOverlayParticle.Get().String(), 1.0f,
							 g_cvarHumanWinOverlaySize.Get(), flRestartDelay,
							 Color(255, 255, 255), g_cvarHumanWinOverlayMaterial.Get().String());
	}
	else if (iTeamNum == CS_TEAM_T)
	{
		if (!g_hTeamT.Get())
		{
			Panic("Cannot find CTeam for T!\n");
			return;
		}
		g_hTeamT->m_iScore = g_hTeamT->m_iScore() + 1;
		if (g_cvarZombieWinOverlayParticle.Get().Length() != 0)
			ZR_CreateOverlay(g_cvarZombieWinOverlayParticle.Get().String(), 1.0f,
							 g_cvarZombieWinOverlaySize.Get(), flRestartDelay,
							 Color(255, 255, 255), g_cvarZombieWinOverlayMaterial.Get().String());
	}
}

CON_COMMAND_CHAT(ztele, "- Teleport to spawn")
{
	// Silently return so the command is completely hidden
	if (!g_cvarEnableZR.Get())
		return;

	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, ZR_PREFIX "You cannot use this command from the server console.");
		return;
	}

	// Check if command is enabled for humans
	if (!g_cvarZteleHuman.Get() && player->m_iTeamNum() == CS_TEAM_CT)
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "You cannot use this command as a human.");
		return;
	}

	std::vector<SpawnPoint*> spawns = ZR_GetSpawns();
	if (!spawns.size())
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "There are no spawns!");
		return;
	}

	// Pick and get random spawnpoint
	int randomindex = rand() % spawns.size();
	CHandle<SpawnPoint> spawnHandle = spawns[randomindex]->GetHandle();

	// Here's where the mess starts
	CBasePlayerPawn* pPawn = player->GetPawn();

	if (!pPawn)
		return;

	if (!pPawn->IsAlive())
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "You cannot teleport when dead!");
		return;
	}

	// Get initial player position so we can do distance check
	Vector initialpos = pPawn->GetAbsOrigin();

	ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "Teleporting to spawn in 5 seconds.");

	CHandle<CCSPlayerPawn> pawnHandle = pPawn->GetHandle();

	new CTimer(5.0f, false, false, [spawnHandle, pawnHandle, initialpos]() {
		CCSPlayerPawn* pPawn = pawnHandle.Get();
		SpawnPoint* pSpawn = spawnHandle.Get();

		if (!pPawn || !pSpawn)
			return -1.0f;

		Vector endpos = pPawn->GetAbsOrigin();

		if (initialpos.DistTo(endpos) < g_cvarMaxZteleDistance.Get())
		{
			Vector origin = pSpawn->GetAbsOrigin();
			QAngle rotation = pSpawn->GetAbsRotation();

			pPawn->Teleport(&origin, &rotation, nullptr);
			ClientPrint(pPawn->GetOriginalController(), HUD_PRINTTALK, ZR_PREFIX "You have been teleported to spawn.");
		}
		else
		{
			ClientPrint(pPawn->GetOriginalController(), HUD_PRINTTALK, ZR_PREFIX "Teleport failed! You moved too far.");
		}

		return -1.0f;
	});
}

CON_COMMAND_CHAT(zclass, "<teamname/class name/number> - Find and select your Z:R classes")
{
	// Silently return so the command is completely hidden
	if (!g_cvarEnableZR.Get())
		return;

	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, ZR_PREFIX "You cannot use this command from the server console.");
		return;
	}

	CUtlVector<std::shared_ptr<ZRClass>> vecClasses;
	int iSlot = player->GetPlayerSlot();
	bool bListingZombie = true;
	bool bListingHuman = true;

	if (args.ArgC() > 1)
	{
		bListingZombie = !V_strcasecmp(args[1], "zombie") || !V_strcasecmp(args[1], "zm") || !V_strcasecmp(args[1], "z");
		bListingHuman = !V_strcasecmp(args[1], "human") || !V_strcasecmp(args[1], "hm") || !V_strcasecmp(args[1], "h");
	}

	g_pZRPlayerClassManager->GetZRClassList(CS_TEAM_NONE, vecClasses, player);

	if (bListingZombie || bListingHuman)
	{
		for (int team = CS_TEAM_T; team <= CS_TEAM_CT; team++)
		{
			if ((team == CS_TEAM_T && !bListingZombie) || (team == CS_TEAM_CT && !bListingHuman))
				continue;

			const char* sTeamName = team == CS_TEAM_CT ? "Human" : "Zombie";
			const char* sCurrentClass = g_pUserPreferencesSystem->GetPreference(iSlot, team == CS_TEAM_CT ? HUMAN_CLASS_KEY_NAME : ZOMBIE_CLASS_KEY_NAME);

			if (sCurrentClass[0] != '\0')
				ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "Your current %s class is: \x10%s\x1. Available classes:", sTeamName, sCurrentClass);
			else
				ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "Available %s classes:", sTeamName);

			FOR_EACH_VEC(vecClasses, i)
			{
				if (vecClasses[i]->iTeam == team)
					ClientPrint(player, HUD_PRINTTALK, "%i. %s", i + 1, vecClasses[i]->szClassName.c_str());
			}
		}

		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "Select a class using \x2!zclass <class name/number>");
		return;
	}

	FOR_EACH_VEC(vecClasses, i)
	{
		const char* sClassName = vecClasses[i]->szClassName.c_str();
		bool bClassMatches = !V_stricmp(sClassName, args[1]) || (V_StringToInt32(args[1], -1, NULL, NULL, PARSING_FLAG_SKIP_WARNING) - 1) == i;
		std::shared_ptr<ZRClass> pClass = vecClasses[i];

		if (bClassMatches)
		{
			ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "Your %s class is now set to \x10%s\x1.", pClass->iTeam == CS_TEAM_CT ? "Human" : "Zombie", sClassName);
			g_pUserPreferencesSystem->SetPreference(iSlot, pClass->iTeam == CS_TEAM_CT ? HUMAN_CLASS_KEY_NAME : ZOMBIE_CLASS_KEY_NAME, sClassName);
			return;
		}
	}

	ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "No available classes matched \x10%s\x1.", args[1]);
}

CON_COMMAND_CHAT_FLAGS(infect, "- Infect a player", ADMFLAG_GENERIC)
{
	// Silently return so the command is completely hidden
	if (!g_cvarEnableZR.Get())
		return;

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "Usage: !infect <name>");
		return;
	}

	if (g_ZRRoundState == EZRRoundState::ROUND_END)
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "The round is already over!");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_TERRORIST | NO_DEAD, nType))
		return;

	const char* pszCommandPlayerName = player ? player->GetPlayerName() : CONSOLE_NAME;
	std::vector<SpawnPoint*> spawns = ZR_GetSpawns();

	if (g_cvarInfectSpawnType.Get() == (int)EZRSpawnType::RESPAWN && !spawns.size())
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "There are no spawns!");
		return;
	}

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);
		CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pTarget->GetPawn();

		if (g_ZRRoundState == EZRRoundState::ROUND_START)
			ZR_InfectMotherZombie(pTarget, spawns);
		else
			ZR_Infect(pTarget, pTarget, true);

		if (iNumClients == 1)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "infected", g_ZRRoundState == EZRRoundState::ROUND_START ? " as a mother zombie" : "", ZR_PREFIX);
	}
	if (iNumClients > 1)
		PrintMultiAdminAction(nType, pszCommandPlayerName, "infected", g_ZRRoundState == EZRRoundState::ROUND_START ? " as mother zombies" : "", ZR_PREFIX);

	// Note we skip MZ immunity when first infection is manually triggered
	if (g_ZRRoundState == EZRRoundState::ROUND_START)
	{
		if (g_cvarRespawnDelay.Get() < 0.0f)
			g_bRespawnEnabled = false;

		g_ZRRoundState = EZRRoundState::POST_INFECTION;
	}
}

CON_COMMAND_CHAT_FLAGS(revive, "- Revive a player", ADMFLAG_GENERIC)
{
	// Silently return so the command is completely hidden
	if (!g_cvarEnableZR.Get())
		return;

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "Usage: !revive <name>");
		return;
	}

	if (g_ZRRoundState != EZRRoundState::POST_INFECTION)
	{
		ClientPrint(player, HUD_PRINTTALK, ZR_PREFIX "A round is not ongoing!");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_DEAD | NO_COUNTER_TERRORIST, nType))
		return;

	const char* pszCommandPlayerName = player ? player->GetPlayerName() : CONSOLE_NAME;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);
		CCSPlayerPawn* pPawn = (CCSPlayerPawn*)pTarget->GetPawn();

		if (!pPawn)
			continue;

		ZR_Cure(pTarget);
		ZR_StripAndGiveKnife(pPawn);

		if (iNumClients == 1)
			PrintSingleAdminAction(pszCommandPlayerName, pTarget->GetPlayerName(), "revived", "", ZR_PREFIX);
	}
	if (iNumClients > 1)
		PrintMultiAdminAction(nType, pszCommandPlayerName, "revived", "", ZR_PREFIX);
}