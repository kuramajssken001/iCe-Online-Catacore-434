/*
 * Copyright (C) 2006-2013 iCe Online <http://ice-wow.eu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptPCH.h"
#include "firelands.h"

/*
 * Author: HyN3Q
 * Completed (in percent): approximately 90%
 */

enum stuffs
{
    SPELL_ARCING_SLASH = 99931,
    SPELL_IMMOLATION_TRAP = 99838, // triggered spell by trap?
    SPELL_IMMOLATION_TRAP_SUMMON = 99839,
    SPELL_SPEAR_TARGET_VISUAL = 99988,
    SPELL_MAGMA_RUPTURE_IMMOLATION = 99841, // need algorithm for archimedean spiral
    SPELL_MAGMA_FLARE_VISUAL = 100495,
    SPELL_CRYSTAL_PRISON_EFFECT = 99837,
    SPELL_CRYSTAL_PRISON_TRAP_SUMMON = 99836, // summon NPC (trap) - need AI!!!
    SPELL_HURL_SPEAR  = 100002,
    SPELL_SUMMON_SPEAR = 99978,
    SPELL_FRENZY = 100522,
    SPELL_MAGMA_RUPTURE = 99840, // to do: add radius index for 45 yd.
    SPELL_LIMB_RIP = 99832,
    SPELL_FRENZIED_DEVOTION = 100064,
    SPELL_FEEDING_FRENZY = 100655,
    SPELL_FACE_RAGE = 99945,
    SPELL_BERSERK = 26662,
    SPELL_DOGGED_DETERMINATION = 101111,
    SPELL_SPEAR_VISUAL = 100026,
    SPELL_WARY = 100167,
    SPELL_SEPARATION_ANXIETY = 99835,

    NPC_CRYSTAL_TRAP = 53713,
    NPC_CRYSTAL_PRISON = 53819,
    NPC_IMMOLATION_TRAP = 53724,
    NPC_IMMOLATION_TRAP_BUNNY = 537240,
    NPC_RAGEFACE = 53695,
    NPC_RIPLIMB = 53694,
};

class boss_shannox : public CreatureScript
{
    public:
        boss_shannox() : CreatureScript("boss_shannox") { }

    struct boss_shannoxAI : public ScriptedAI
    {
        boss_shannoxAI(Creature* c) : ScriptedAI(c)
        {
            if (c->GetInstanceScript())
                pInstance = c->GetInstanceScript();
        }

        Creature* pRiplimb;
        std::list<uint64> summonedTraps;
        Creature* pRageface;
        InstanceScript* pInstance;
        uint32 ArcingSlashTimer;
        uint32 CrystalTrapTimer;
        uint32 ImmolationTrapTimer;
        uint32 HurlSpearTimer;
        uint32 checkEverySecond;

        void Reset()
        {
            pRiplimb = NULL;
            pRageface = NULL;
            checkEverySecond = 1000;
            ArcingSlashTimer = 7000;
            CrystalTrapTimer = 8000;
            ImmolationTrapTimer = 10000;
            HurlSpearTimer = 20000;

            me->SetReactState(REACT_PASSIVE);

            DespawnTraps();
            summonedTraps.clear();

            if (pInstance)
            {
                pInstance->SetData(TYPE_SHANNOX, NOT_STARTED);

                if (pRiplimb == NULL)
                    pRiplimb = me->GetCreature(*me, pInstance->GetData64(DATA_RIPLIMB_GUID));

                if (pRageface == NULL)
                    pRageface = me->GetCreature(*me, pInstance->GetData64(DATA_RAGEFACE_GUID));
            }
        }

        void DespawnTraps()
        {
            if (!summonedTraps.empty())
            {
                for (std::list<uint64>::const_iterator itr = summonedTraps.begin(); itr != summonedTraps.end(); itr++)
                {
                    Creature* pTraps = NULL;

                    if ((pTraps = me->GetMap()->GetCreature((*itr))) != NULL)
                        pTraps->ForcedDespawn(5000);
                }
            }
        }

        void JustSummoned(Creature* pSummon)
        {
            switch (pSummon->GetEntry())
            {
                case NPC_CRYSTAL_TRAP:
                case NPC_IMMOLATION_TRAP:
                    summonedTraps.push_back(pSummon->GetGUID());
                    break;
                case 53752: // Hurl Spear weapon
                {
                    pSummon->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_DISABLE_MOVE);
                    pSummon->setFaction(35);
                    Creature* bunny = pSummon->SummonCreature(NPC_IMMOLATION_TRAP_BUNNY, pSummon->GetPositionX(), pSummon->GetPositionY(), pSummon->GetPositionZ());
                    if (bunny)
                    {
                        bunny->CastSpell(bunny, SPELL_SPEAR_TARGET_VISUAL, true);
                        summonedTraps.push_back(bunny->GetGUID());
                    }
                    summonedTraps.push_back(pSummon->GetGUID());
                    break;
                }
                default:
                    break;
            }
        }

        void SummonedCreatureDespawn(Creature* pSummon)
        {
            switch (pSummon->GetEntry())
            {
                case 53752:
                {
                    if (!summonedTraps.empty())
                    {
                        for (std::list<uint64>::const_iterator itr = summonedTraps.begin(); itr != summonedTraps.end(); itr++)
                        {
                            Creature* bunny = NULL;

                            if ((bunny = me->GetMap()->GetCreature((*itr))) != NULL)
                                if (bunny->GetEntry() == NPC_IMMOLATION_TRAP_BUNNY)
                                    bunny->ForcedDespawn();
                        }
                    }
                }
                    break;
            }
        }

        void JustDied(Unit* killer)
        {
            if (pInstance)
                pInstance->SetData(TYPE_SHANNOX, DONE);

            DoYell("The pain... Lord of fire, it hurts...", 0);
        }

        void KilledUnit(Unit* pTarget)
        {
            if (pTarget->GetTypeId() == TYPEID_PLAYER)
            {
                switch (urand(0,3))
                {
                    case 0:
                        DoYell("Yes... oh yes!", 0);
                        break;
                    case 1:
                        DoYell("The Firelord will be most pleased!", 0);
                        break;
                    case 2:
                        DoYell("Now you stay dead!", 0);
                        break;
                    case 3:
                        DoYell("Dog food!", 0);
                        break;
                }
            }
        }

        void SpellHitTarget(Unit* target, const SpellEntry* spell)
        {
            if (spell->Id == SPELL_HURL_SPEAR)
            {
                me->CastSpell(pRiplimb, SPELL_SUMMON_SPEAR, true);
                pRiplimb->AI()->DoAction(1);
            }
        }

        void DoYell(const char* text, uint32 soundid)
        {
            if (text)
                me->MonsterYell(text, LANG_UNIVERSAL, 0);

            if (soundid)
                me->PlayDirectSound(soundid);
        }

        void EnterCombat(Unit* pWho)
        {
            if (pInstance)
            {
                pInstance->SetData(TYPE_SHANNOX, IN_PROGRESS);
            }

            DoYell("Aha! The interlopers... Kill them! EAT THEM!", 0);

            if (pRiplimb)
            {
                if (Unit* pTarget = SelectUnit(SELECT_TARGET_RANDOM, 0))
                {
                    pRiplimb->MonsterSay("TEST", 0,0);
                    pRiplimb->AI()->AttackStart(pTarget);
                }
            }

            if (pRageface)
            {
                if (Unit* pTarget = SelectUnit(SELECT_TARGET_RANDOM, 0))
                {
                    pRageface->AI()->AttackStart(pTarget);
                    pRageface->Attack(pTarget, true);
                }
            }

            me->SetReactState(REACT_AGGRESSIVE);
        }

        void DoAction(const int32 action)
        {
            // Riblimb dies..
            if (action == 1)
            {
                me->CastSpell(me, SPELL_FRENZY, false);
                DoYell("Riplimb! No... no! Oh, you terrible little beasts! HOW COULD YOU?!", 0);
            }
            // rageface dies..
            if (action == 2)
            {
                me->CastSpell(me, SPELL_FRENZY, false);
                DoYell("You murderers! Why... why would you kill such a noble animal?!", 0);
            }
        }

        void UpdateAI(const uint32 diff)
        {
            if (!UpdateVictim())
                return;

            if (ArcingSlashTimer <= diff)
            {
                if (!me->IsNonMeleeSpellCasted(false))
                {
                    me->CastSpell(me->getVictim(), SPELL_ARCING_SLASH, false);
                    ArcingSlashTimer = 11000;
                }
            } else ArcingSlashTimer -= diff;

            if (CrystalTrapTimer <= diff)
            {
                if (!me->IsNonMeleeSpellCasted(false))
                {
                    Unit* pTarget = SelectTarget(SELECT_TARGET_RANDOM, 0, 1000, true);

                    if (pTarget)
                        me->CastSpell(pTarget, SPELL_CRYSTAL_PRISON_TRAP_SUMMON, false);

                    CrystalTrapTimer = 25500;
                }
            } else CrystalTrapTimer -= diff;

            if (ImmolationTrapTimer <= diff)
            {
                if (!me->IsNonMeleeSpellCasted(false))
                {
                    Unit* pTarget = SelectTarget(SELECT_TARGET_RANDOM, 0, 1000, true);

                    if (pTarget)
                        me->CastSpell(pTarget, SPELL_IMMOLATION_TRAP_SUMMON, false);

                    ImmolationTrapTimer = 10000;
                }

            } else ImmolationTrapTimer -= diff;

            if (HurlSpearTimer <= diff)
            {
                if (!me->IsNonMeleeSpellCasted(false))
                {
                    if (pRiplimb->isAlive())
                        me->CastSpell(pRiplimb, SPELL_HURL_SPEAR, false);
                    else {
                        if (IsHeroic()) {
                            if (Unit* pTarget = SelectTarget(SELECT_TARGET_RANDOM, 0, 100, true))
                                me->CastSpell(pTarget, SPELL_MAGMA_RUPTURE, false);
                        }
                    }

                    HurlSpearTimer = 42000;
                }
            } else HurlSpearTimer -= diff;

            if (checkEverySecond <= diff)
            {
                if (me->GetDistance(pRiplimb) >= 60.0f)
                {
                    me->CastSpell(me, SPELL_SEPARATION_ANXIETY, false);
                    pRiplimb->CastSpell(pRiplimb, SPELL_SEPARATION_ANXIETY, false);
                    checkEverySecond = 3000;
                }
                else
                    checkEverySecond = 1000;
            } else checkEverySecond -= diff;

            DoMeleeAttackIfReady();

        }
    };

    CreatureAI* GetAI(Creature* c) const
    {
        return new boss_shannoxAI(c);
    }
};

class npc_crystal_trap : public CreatureScript
{
    public:
        npc_crystal_trap() : CreatureScript("npc_crystal_trap") { }

    struct npc_crystal_trapAI : public ScriptedAI
    {
        npc_crystal_trapAI(Creature* c) : ScriptedAI(c)
        {
            c->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_DISABLE_MOVE);
            c->setFaction(35);
        }


        InstanceScript* pInstance;
        bool armTrap;
        bool trapActivated;
        uint32 armtrapTimer;
        uint64 prisonerGuid;
        Creature* pBoss;

        void Reset()
        {
            armTrap = false;
            trapActivated = false;
            prisonerGuid = 0;
            pBoss = NULL;

            if (me->GetInstanceScript())
                pInstance = me->GetInstanceScript();
            else
                pInstance = NULL;

            if (pInstance)
            {
                if (pBoss == NULL)
                    pBoss = Unit::GetCreature(*me, me->GetInstanceScript()->GetData64(TYPE_SHANNOX));

                if (pBoss)
                    armtrapTimer = 2000 + uint32(me->GetDistance2d(pBoss) / 100) * 1000;
            }
            else
                armtrapTimer = 2000;
        }

        void MoveInLineOfSight(Unit* pWho)
        {
            if (!pWho || pWho->GetDistance(me) > 0.3f || armTrap == false || trapActivated == true || pWho->HasAura(SPELL_WARY))
                return;

            if (pWho->GetTypeId() == TYPEID_PLAYER || (pWho->GetCharmerOrOwnerPlayerOrPlayerItself() == NULL
                && !pWho->HasAura(SPELL_SPEAR_VISUAL) && (pWho->GetEntry() == NPC_RIPLIMB || pWho->GetEntry() == NPC_RAGEFACE)))
                ActivateTrap(pWho);

            ScriptedAI::MoveInLineOfSight(pWho);
        }

        void ActivateTrap(Unit* pWho)
        {
            if (pWho)
            {
                prisonerGuid = pWho->GetGUID();
                trapActivated = true;

                if (pBoss)
                {
                    pBoss->CastSpell(pWho, SPELL_CRYSTAL_PRISON_EFFECT, false);
                    float x,y,z;
                    pWho->GetPosition(x, y, z);
                    pWho->NearTeleportTo(x,y,z,0,false);
                    me->SummonCreature(NPC_CRYSTAL_PRISON, x, y, z, 0, TEMPSUMMON_CORPSE_DESPAWN);
                    DoUpdateObjectVisibility();
                }

                me->SetVisibility(VISIBILITY_OFF);
            }
        }

        void DoUpdateObjectVisibility()
        {
            Map::PlayerList const& plrList = me->GetMap()->GetPlayers();

            if (!plrList.isEmpty())
            {
                for (Map::PlayerList::const_iterator itr = plrList.begin(); itr != plrList.end(); ++itr)
                {
                    itr->getSource()->UpdateObjectVisibility();
                }
            }
        }

        Unit* GetPrisoner()
        {
            if (Unit* prisoner = Unit::GetUnit(*me, prisonerGuid))
                return prisoner;
            else
                return NULL;
        }

        void DoAction(const int32 id)
        {
            if (id == 1)
            {
                if (prisonerGuid != 0)
                {
                    if (GetPrisoner())
                    {
                        if (GetPrisoner()->GetEntry() == NPC_RIPLIMB || GetPrisoner()->GetEntry() == NPC_RAGEFACE)
                            GetPrisoner()->CastSpell(GetPrisoner(), SPELL_WARY, true);

                        if (GetPrisoner()->HasAura(SPELL_FEEDING_FRENZY))
                            GetPrisoner()->RemoveAurasDueToSpell(SPELL_FEEDING_FRENZY);

                        GetPrisoner()->RemoveAurasDueToSpell(SPELL_CRYSTAL_PRISON_EFFECT);
                        me->ForcedDespawn();
                    }
                }
            }
        }

        void UpdateAI(const uint32 diff)
        {
            if (armTrap == false && trapActivated == false) // done
            {
                if (armtrapTimer <= diff)
                {
                    armTrap = true;
                    armtrapTimer = 2000;
                } else armtrapTimer -= diff;
            }
        }
    };

    CreatureAI* GetAI(Creature* c) const
    {
        return new npc_crystal_trapAI(c);
    }
};

class npc_crystal_prison : public CreatureScript
{
    public:
        npc_crystal_prison() : CreatureScript("npc_crystal_prison") { }

    struct npc_crystal_prisonAI : public ScriptedAI
    {
        npc_crystal_prisonAI(Creature* c) : ScriptedAI(c)
        {
            c->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
            c->SetReactState(REACT_PASSIVE);
            summonerGuid = 0;
        }

        uint32 selfKillTimer;
        uint64 summonerGuid;

        void Reset()
        {
            selfKillTimer = 1000;
        }

        void IsSummonedBy(Unit* pSummoner)
        {
            if (pSummoner)
                summonerGuid = pSummoner->GetGUID();
        }

        void UpdateAI(const uint32 diff)
        {
            if (Creature* pTrap = Unit::GetCreature(*me, summonerGuid))
            {
                if (((npc_crystal_trap::npc_crystal_trapAI*)pTrap->GetAI())->GetPrisoner())
                {
                    if (!((npc_crystal_trap::npc_crystal_trapAI*)pTrap->GetAI())->GetPrisoner()->isAlive())
                        me->Kill(me);

                    if (((npc_crystal_trap::npc_crystal_trapAI*)pTrap->GetAI())->GetPrisoner()->GetTypeId() == TYPEID_UNIT &&
                        (((npc_crystal_trap::npc_crystal_trapAI*)pTrap->GetAI())->GetPrisoner()->GetEntry() == NPC_RIPLIMB ||
                        ((npc_crystal_trap::npc_crystal_trapAI*)pTrap->GetAI())->GetPrisoner()->GetEntry() == NPC_RAGEFACE))
                    {
                        if (selfKillTimer <= diff)
                        {
                            me->DealDamage(me, me->CountPctFromMaxHealth(10), 0, DIRECT_DAMAGE);
                            selfKillTimer = 1000;
                        } else selfKillTimer -= diff;
                    }
                }

            }
        }

        void JustDied(Unit* pKiller)
        {
            if (Creature* pTrap = Unit::GetCreature(*me, summonerGuid))
            {
                pTrap->AI()->DoAction(1);
                me->ForcedDespawn();
                summonerGuid = 0;
            }
        }
    };

    CreatureAI* GetAI(Creature* c) const
    {
        return new npc_crystal_prisonAI(c);
    }
};

class npc_immolation_trap : public CreatureScript
{
    public:
        npc_immolation_trap() : CreatureScript("npc_immolation_trap") { }

    struct npc_immolation_trapAI : public ScriptedAI
    {
        npc_immolation_trapAI(Creature* c) : ScriptedAI(c)
        {
            c->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_DISABLE_MOVE);
            c->setFaction(35);
        }

        bool armTrap;
        InstanceScript* pInstance;
        bool trapActivated;
        bool doeffect;
        uint32 armtrapTimer;
        uint32 activatingTrapTimer;
        Creature* pBoss;

        void Reset()
        {
            doeffect = false;
            armTrap = false;
            trapActivated = false;
            doeffect = false;
            pBoss = NULL;

            if (me->GetInstanceScript())
                pInstance = me->GetInstanceScript();

            if (pInstance)
            {
                if (pBoss == NULL)
                    pBoss = Unit::GetCreature(*me, pInstance->GetData64(TYPE_SHANNOX));

                if (pBoss)
                    armtrapTimer = 2000 + uint32(me->GetDistance2d(pBoss) / 100) * 1000;
            }
            else
                armtrapTimer = 2000;

            activatingTrapTimer = 500;
        }

        void MoveInLineOfSight(Unit* pWho)
        {
            if (!pWho || pWho->GetDistance(me) > 0.3f || armTrap == false || trapActivated == true)
                return;

            if (pWho->GetTypeId() == TYPEID_PLAYER || (pWho->GetCharmerOrOwnerPlayerOrPlayerItself() == NULL
                && (pWho->GetEntry() == NPC_RIPLIMB || pWho->GetEntry() == NPC_RAGEFACE)))
                ActivateTrap(pWho);

            ScriptedAI::MoveInLineOfSight(pWho);
        }

        void ActivateTrap(Unit* pWho)
        {
            if (pWho)
            {
                trapActivated = true;
                doeffect = true;
            }
        }

        void UpdateAI(const uint32 diff)
        {
            if (armTrap == false && trapActivated == false) // done
            {
                if (armtrapTimer <= diff)
                {
                    armTrap = true;
                    armtrapTimer = 2000;
                } else armtrapTimer -= diff;
            }

            if (doeffect == true)
            {
                if (activatingTrapTimer <= diff)
                {
                    Creature* pSummon = me->SummonCreature(NPC_IMMOLATION_TRAP_BUNNY, me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
                    if (pSummon)
                    {
                        pSummon->SetVisibility(VISIBILITY_ON);
                        pSummon->CastSpell(pSummon, SPELL_MAGMA_FLARE_VISUAL, true);
                        pSummon->setFaction(35);
                        float x, y, z, angle, xc, yc;
                        pSummon->GetPosition(xc, yc, z);
                        me->SetVisibility(VISIBILITY_OFF);

                        for (uint32 i = 0; i < 190; i += 2)
                        {
                           angle = i * 0.1f;

                           x = xc + (1 + 2.3f * angle) * cos(angle);
                           y = yc + (1 + 2.3f * angle) * sin(angle);
                           pBoss->CastSpell(x,y,z, SPELL_MAGMA_RUPTURE_IMMOLATION, true);
                        }

                        me->ForcedDespawn(1000);
                        pSummon->ForcedDespawn(1000);
                    }
                    activatingTrapTimer = 500;
                    doeffect = false;
                }
                else activatingTrapTimer -= diff;
            }
        }

    };

    CreatureAI* GetAI(Creature* c) const
    {
        return new npc_immolation_trapAI(c);
    }
};

class npc_riplimb : public CreatureScript
{
    public:
        npc_riplimb() : CreatureScript("npc_riplimb") { }

    struct npc_riplimbAI : public ScriptedAI
    {
        npc_riplimbAI(Creature* c) : ScriptedAI(c)
        {
        }

        InstanceScript* pInstance;
        bool spearDropped;
        bool shannoxHP;
        bool riplimbHP;
        uint32 spearDelayTimer;
        uint32 limbRipTimer;
        uint8 bossWait;
        Unit* pSpear;
        Creature* pBoss;

        void Reset()
        {
            pSpear = NULL;
            pBoss = NULL;
            spearDropped = false;
            shannoxHP = false;
            riplimbHP = false;
            spearDelayTimer = 3000;
            limbRipTimer = urand(8000, 10000);
            bossWait = 0;
            if (me->GetInstanceScript())
            {
                pInstance = me->GetInstanceScript();

                if (pBoss == NULL)
                    pBoss = Unit::GetCreature(*me, pInstance->GetData64(TYPE_SHANNOX));
            }
            else
                pInstance = NULL;
        }


        void EnterCombat(Unit* target)
        {
        }

        void MovementInform(uint32 type, uint32 id)
        {
            if (type != POINT_MOTION_TYPE)
                return;

            if (id == 1)
            {
                if (pSpear)
                {
                    pSpear->ToCreature()->ForcedDespawn();
                    me->CastSpell(me, SPELL_SPEAR_VISUAL, true);
                    bossWait = 2;
                }
            }
            if (id == 2)
            {
                bossWait = 0;
                me->RemoveAurasDueToSpell(SPELL_SPEAR_VISUAL);
            }
        }

        void UpdateAI(const uint32 diff)
        {
            if (!UpdateVictim())
                return;

            if (bossWait > 0)
            {
                if (bossWait == 1) {
                    if (pBoss)
                        me->GetMotionMaster()->MovePoint(2, pBoss->GetPositionX(), pBoss->GetPositionY(), pBoss->GetPositionZ());
                }

                if (bossWait == 2) {
                    me->GetMotionMaster()->MoveChase(me->getVictim());
                }
            }

            if (spearDropped == true)
            {
                if (spearDelayTimer <= diff)
                {
                    if ((pSpear = GetClosestCreatureWithEntry(me, 53752, 100)) != NULL)
                    {
                        me->GetMotionMaster()->MovePoint(1, pSpear->GetPositionX(), pSpear->GetPositionY(), pSpear->GetPositionZ());
                        spearDropped = false;
                        spearDelayTimer = 3000;
                    }
                } else spearDelayTimer -= diff;
            }

            if (limbRipTimer <= diff)
            {
                me->CastSpell(me->getVictim(), SPELL_LIMB_RIP, false);
                limbRipTimer = urand(8000, 10000);
            }
            else limbRipTimer -= diff;

            if (!IsHeroic())
            {
                if ((pBoss && pBoss->HealthBelowPct(30) && !shannoxHP) || (!riplimbHP && me->HealthBelowPct(30)))
                {
                    me->CastSpell(me, SPELL_FRENZIED_DEVOTION, false);
                    riplimbHP = true;
                    shannoxHP = true;
                }

                DoMeleeAttackIfReady();
            }
            else
            {
                DoMeleeAttackIfReady();
                DoSpellAttackIfReady(SPELL_FEEDING_FRENZY);
            }
        }

        void DoAction(const int32 action)
        {
            if (action == 1) {
                spearDropped = true;
            }
        }

        void JustRespawned()
        {
            if (pBoss && pBoss->isAlive())
                me->SetPosition(pBoss->GetPositionX(), pBoss->GetPositionY(), pBoss->GetPositionZ(), pBoss->GetOrientation(), true);
        }

        void JustDied(Unit* pKiller)
        {
            if (pBoss)
            {
                pBoss->AI()->DoAction(1);

                if (pBoss->isAlive())
                    me->SetRespawnTime(30);
            }
        }
    };

    CreatureAI* GetAI(Creature* c) const
    {
        return new npc_riplimbAI(c);
    }
};

class npc_rageface : public CreatureScript
{
    public:
        npc_rageface() : CreatureScript("npc_rageface") { }

    struct npc_ragefaceAI : public ScriptedAI
    {
        npc_ragefaceAI(Creature* c) : ScriptedAI(c)
        {
            c->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_ATTACK_ME, true);
            c->ApplySpellImmune(0, IMMUNITY_ID, 49560, true); // Death Grip jump effect
            c->ApplySpellImmune(0, IMMUNITY_ID, 81261, true); // Solar Beam
            c->ApplySpellImmune(0, IMMUNITY_ID, 88625, true); // Chastise
        }

        InstanceScript* pInstance;
        uint32 changeTargetTimer;
        bool ragefaceHP;

        void Reset()
        {
            ragefaceHP = true;
            changeTargetTimer = urand(8000, 10000);
            if (me->GetInstanceScript())
                pInstance = me->GetInstanceScript();
            else
                pInstance = NULL;
        }

        void AttackStart(Unit* pWho)
        {
            if (me->getVictim())
                me->AddThreat(me->getVictim(), 10000.0f);
        }

        void UpdateAI(const uint32 diff)
        {
            if (!UpdateVictim())
                return;

            if (changeTargetTimer <= diff)
            {
                // little hack
                if (Unit* pTarget = SelectUnit(SELECT_TARGET_RANDOM, 0))
                {
                    me->getVictim()->getHostileRefManager().deleteReference(me);
                    pTarget->getHostileRefManager().deleteReference(me); // for safety..
                    me->SetUInt64Value(UNIT_FIELD_TARGET, pTarget->GetGUID());
                    me->CastSpell(pTarget, SPELL_FACE_RAGE, false);
                    me->AddThreat(pTarget, 10000.0f);
                }
                changeTargetTimer = urand(8000, 10000);
            }
            else changeTargetTimer -= diff;

            if (!IsHeroic())
            {
                if (!ragefaceHP && me->HealthBelowPct(30))
                {
                    me->CastSpell(me, SPELL_FRENZIED_DEVOTION, false);
                    ragefaceHP = true;
                }

                DoMeleeAttackIfReady();
            }
            else
            {
                DoMeleeAttackIfReady();
                DoSpellAttackIfReady(SPELL_FEEDING_FRENZY);
            }
        }

        void JustDied(Unit* pKiller)
        {
            if (Creature* pBoss = Unit::GetCreature(*me, pInstance->GetData64(TYPE_SHANNOX)))
                pBoss->AI()->DoAction(2);
        }
    };

    CreatureAI* GetAI(Creature* c) const
    {
        return new npc_ragefaceAI(c);
    }
};

void AddSC_boss_shannox()
{
    new boss_shannox();
    new npc_crystal_trap();
    new npc_crystal_prison();
    new npc_immolation_trap();
    new npc_riplimb();
    new npc_rageface();
}

/*
SQL:
UPDATE creature_template SET scriptname = "npc_crystal_trap" WHERE entry = 53713;
UPDATE creature_template SET scriptname = "npc_crystal_prison" WHERE entry = 53819;
UPDATE creature_template SET scriptname = "npc_immolation_trap" WHERE entry = 53724;
UPDATE creature_template SET scriptname = "npc_riplimb" WHERE entry = 53694;
UPDATE creature_template SET scriptname = "npc_rageface" WHERE entry = 53695;

INSERT INTO creature_template
  (entry, difficulty_entry_1, difficulty_entry_2, difficulty_entry_3, KillCredit1, KillCredit2, modelid1, modelid2, modelid3, modelid4, name, subname, IconName, gossip_menu_id, minlevel, maxlevel, exp, faction_A, faction_H, npcflag, speed_walk, speed_run, scale, rank, mindmg, maxdmg, dmgschool, attackpower, dmg_multiplier, baseattacktime, rangeattacktime, unit_class, unit_flags, dynamicflags, family, trainer_type, trainer_spell, trainer_class, trainer_race, minrangedmg, maxrangedmg, rangedattackpower, type, type_flags, lootid, pickpocketloot, skinloot, resistance1, resistance2, resistance3, resistance4, resistance5, resistance6, spell1, spell2, spell3, spell4, spell5, spell6, spell7, spell8, PetSpellDataId, VehicleId, mingold, maxgold, AIName, MovementType, InhabitType, Health_mod, Mana_mod, Armor_mod, RacialLeader, questItem1, questItem2, questItem3, questItem4, questItem5, questItem6, movementId, RegenHealth, equipment_id, mechanic_immune_mask, flags_extra, ScriptName, WDBVerified)
VALUES
  (537240, 0, 0, 0, 0, 0, 11686, 0, 0, 0, "Immolation Trap trigger", "NULL", "NULL", 0, 1, 1, 3, 35, 35, 0, 1, "1,14286", "0,3", 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "", 0, 3, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, "", 15595);

*/