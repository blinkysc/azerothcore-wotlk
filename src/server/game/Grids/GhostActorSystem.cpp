/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "GhostActorSystem.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "ThreatMgr.h"
#include "Unit.h"
#include "WorkStealingPool.h"
#include "World.h"
#include "WorldPacket.h"
#include <thread>

namespace GhostActor
{

// ============================================================================
// CellActor Implementation
// ============================================================================

void CellActor::Update(uint32_t diff)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    _lastUpdateTime += diff;
    ProcessMessages();
    UpdateEntities(diff);

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    _lastUpdateUs.store(static_cast<uint64_t>(duration.count()), std::memory_order_relaxed);
}

void CellActor::ProcessMessages()
{
    ActorMessage msg;
    while (_inbox.Pop(msg))
    {
        HandleMessage(msg);
    }
}

void CellActor::HandleMessage(ActorMessage& msg)
{
    IncrementMessageCount();

    // Record message type for performance monitoring
    if (_map && _map->GetCellActorManager())
        _map->GetCellActorManager()->GetPerfStats().RecordMessage(msg.type);

    switch (msg.type)
    {
        case MessageType::SPELL_HIT:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<SpellHitPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: SPELL_HIT spell={} target={} damage={} healing={}",
                    _cellId, payload->spellId, msg.targetGuid, payload->damage, payload->healing);

                WorldObject* targetObj = FindEntityByGuid(msg.targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    // Apply damage if present
                    if (payload->damage > 0)
                    {
                        Unit::DealDamage(nullptr, target, payload->damage, nullptr,
                            SPELL_DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                    }
                    // Apply healing if present
                    if (payload->healing > 0)
                    {
                        target->ModifyHealth(payload->healing);
                    }
                    // Broadcast health change to ghosts in neighboring cells
                    BroadcastHealthChange(target);
                }
            }
            break;
        }

        case MessageType::MELEE_DAMAGE:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<MeleeDamagePayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: MELEE_DAMAGE target={} damage={} crit={} attacker={}",
                    _cellId, msg.targetGuid, payload->damage, payload->isCritical, msg.sourceGuid);

                WorldObject* targetObj = FindEntityByGuid(msg.targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    // Apply the incoming melee damage
                    Unit::DealDamage(nullptr, target, payload->damage, nullptr,
                        DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                    BroadcastHealthChange(target);

                    // Process damage shields (thorns) - send reflect damage back to attacker
                    if (msg.sourceGuid != 0 && msg.sourceCellId != 0 && payload->damage > 0)
                    {
                        // Get damage shield auras on the target (victim with thorns)
                        Unit::AuraEffectList vDamageShields = target->GetAuraEffectsByType(SPELL_AURA_DAMAGE_SHIELD);
                        for (AuraEffect const* dmgShield : vDamageShields)
                        {
                            SpellInfo const* spellProto = dmgShield->GetSpellInfo();
                            if (!spellProto)
                                continue;

                            // Get base damage from the aura
                            uint32 reflectDamage = uint32(std::max(0, dmgShield->GetAmount()));
                            if (reflectDamage == 0)
                                continue;

                            // Apply spell damage bonus from the aura caster if available
                            if (Unit* auraCaster = dmgShield->GetCaster())
                            {
                                // We don't have direct access to the attacker for SpellDamageBonusDone,
                                // but we can at least apply caster bonuses
                                reflectDamage = auraCaster->SpellDamageBonusDone(target, spellProto,
                                    reflectDamage, SPELL_DIRECT_DAMAGE, dmgShield->GetEffIndex());
                            }

                            LOG_DEBUG("server.ghost", "CellActor[{}]: Processing thorns spell={} damage={} to attacker={}",
                                _cellId, spellProto->Id, reflectDamage, msg.sourceGuid);

                            // Send reflect damage message to attacker's cell
                            auto* cellMgr = _map->GetCellActorManager();
                            if (cellMgr)
                            {
                                auto reflectPayload = std::make_shared<ReflectDamagePayload>();
                                reflectPayload->reflectorGuid = msg.targetGuid;
                                reflectPayload->attackerGuid = msg.sourceGuid;
                                reflectPayload->spellId = spellProto->Id;
                                reflectPayload->damage = reflectDamage;
                                reflectPayload->schoolMask = spellProto->GetSchoolMask();
                                reflectPayload->absorb = 0;  // Calculated on receive
                                reflectPayload->resist = 0;  // Calculated on receive

                                ActorMessage reflectMsg{};
                                reflectMsg.type = MessageType::REFLECT_DAMAGE;
                                reflectMsg.sourceGuid = msg.targetGuid;
                                reflectMsg.targetGuid = msg.sourceGuid;
                                reflectMsg.sourceCellId = _cellId;
                                reflectMsg.targetCellId = msg.sourceCellId;
                                reflectMsg.complexPayload = reflectPayload;

                                cellMgr->SendMessage(msg.sourceCellId, std::move(reflectMsg));
                            }
                        }
                    }
                }
            }
            break;
        }

        case MessageType::HEAL:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<HealPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: HEAL spell={} target={} amount={}",
                    _cellId, payload->spellId, msg.targetGuid, payload->healAmount);

                WorldObject* targetObj = FindEntityByGuid(msg.targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    target->ModifyHealth(payload->effectiveHeal);
                    BroadcastHealthChange(target);
                }
            }
            break;
        }

        case MessageType::REFLECT_DAMAGE:
        {
            // Bi-directional thorns/damage shield response
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<ReflectDamagePayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: REFLECT_DAMAGE spell={} attacker={} damage={}",
                    _cellId, payload->spellId, payload->attackerGuid, payload->damage);

                WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
                Unit* attacker = attackerObj ? attackerObj->ToUnit() : nullptr;

                if (attacker && attacker->IsAlive() && attacker->IsInWorld())
                {
                    uint32 damage = payload->damage;
                    SpellInfo const* spellProto = sSpellMgr->GetSpellInfo(payload->spellId);
                    uint32 absorb = 0;
                    uint32 resist = 0;

                    // Check immunity
                    if (spellProto && attacker->IsImmunedToDamageOrSchool(spellProto))
                    {
                        LOG_DEBUG("server.ghost", "CellActor[{}]: REFLECT_DAMAGE immune to spell {}",
                            _cellId, payload->spellId);
                        break;
                    }

                    // Calculate absorb/resist on the attacker
                    if (spellProto && damage > 0)
                    {
                        // Create a fake "reflector" unit reference for absorb calc
                        // We use nullptr for attacker in DamageInfo since we don't have the reflector
                        DamageInfo dmgInfo(nullptr, attacker, damage, spellProto,
                            SpellSchoolMask(payload->schoolMask), SPELL_DIRECT_DAMAGE);
                        Unit::CalcAbsorbResist(dmgInfo);
                        absorb = dmgInfo.GetAbsorb();
                        resist = dmgInfo.GetResist();
                        damage = dmgInfo.GetDamage();
                    }

                    Unit::DealDamageMods(attacker, damage, &absorb);

                    // Send combat log packet
                    if (spellProto)
                    {
                        WorldPacket data(SMSG_SPELLDAMAGESHIELD, (8 + 8 + 4 + 4 + 4 + 4));
                        data << ObjectGuid(payload->reflectorGuid);  // Reflector (who has thorns)
                        data << attacker->GetGUID();                  // Attacker (receiving damage)
                        data << uint32(payload->spellId);
                        data << uint32(damage);
                        int32 overkill = int32(damage) - int32(attacker->GetHealth());
                        data << uint32(overkill > 0 ? overkill : 0);
                        data << uint32(payload->schoolMask);
                        attacker->SendMessageToSet(&data, true);
                    }

                    // Apply the reflect damage
                    if (damage > 0)
                    {
                        Unit::DealDamage(nullptr, attacker, damage, nullptr,
                            SPELL_DIRECT_DAMAGE, SpellSchoolMask(payload->schoolMask), spellProto, true);
                        BroadcastHealthChange(attacker);
                    }
                }
            }
            break;
        }

        case MessageType::ENTITY_ENTERING:
        {
            // Entity is transferring to this cell
            LOG_DEBUG("server.ghost", "CellActor[{}]: Entity {} entering from cell {}",
                _cellId, msg.sourceGuid, msg.sourceCellId);
            break;
        }

        case MessageType::ENTITY_LEAVING:
        {
            // Entity is leaving this cell
            LOG_DEBUG("server.ghost", "CellActor[{}]: Entity {} leaving to cell {}",
                _cellId, msg.sourceGuid, msg.targetCellId);
            break;
        }

        case MessageType::POSITION_UPDATE:
        {
            // Update ghost position
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncPosition(msg.floatParam1, msg.floatParam2, msg.floatParam3, 0.0f);
            }
            break;
        }

        case MessageType::HEALTH_CHANGED:
        {
            // Update ghost's health display
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncHealth(
                    static_cast<uint32_t>(msg.intParam1),
                    static_cast<uint32_t>(msg.intParam2));
            }
            break;
        }

        case MessageType::COMBAT_STATE_CHANGED:
        {
            // Update ghost's combat state
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncCombatState(msg.intParam1 != 0);
            }
            break;
        }

        case MessageType::GHOST_CREATE:
        {
            // Create a ghost for an entity in neighboring cell
            if (_ghosts.find(msg.sourceGuid) == _ghosts.end())
            {
                auto ghost = std::make_unique<GhostEntity>(msg.sourceGuid, msg.sourceCellId);

                // If we have a full snapshot in complexPayload, use it
                if (msg.complexPayload)
                {
                    auto snapshot = std::static_pointer_cast<GhostSnapshot>(msg.complexPayload);
                    ghost->SyncFromSnapshot(*snapshot);
                }
                else
                {
                    // Use basic params
                    ghost->SyncPosition(msg.floatParam1, msg.floatParam2, msg.floatParam3, 0.0f);
                    ghost->SyncHealth(
                        static_cast<uint32_t>(msg.intParam1),
                        static_cast<uint32_t>(msg.intParam2));
                }

                _ghosts[msg.sourceGuid] = std::move(ghost);
            }
            break;
        }

        case MessageType::GHOST_UPDATE:
        {
            // Full state sync for a ghost
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end() && msg.complexPayload)
            {
                auto snapshot = std::static_pointer_cast<GhostSnapshot>(msg.complexPayload);
                it->second->SyncFromSnapshot(*snapshot);
            }
            break;
        }

        case MessageType::GHOST_DESTROY:
        {
            // Remove ghost - entity no longer visible
            _ghosts.erase(msg.sourceGuid);
            break;
        }

        case MessageType::AURA_APPLY:
        {
            uint32_t spellId = static_cast<uint32_t>(msg.intParam1);
            uint8_t effectMask = static_cast<uint8_t>(msg.intParam2);

            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncAuraApplied(spellId, effectMask);
                LOG_DEBUG("server.ghost", "CellActor[{}]: AURA_APPLY entity={} spell={} effectMask={}",
                    _cellId, msg.sourceGuid, spellId, effectMask);
            }
            break;
        }

        case MessageType::AURA_REMOVE:
        {
            uint32_t spellId = static_cast<uint32_t>(msg.intParam1);

            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncAuraRemoved(spellId);
                LOG_DEBUG("server.ghost", "CellActor[{}]: AURA_REMOVE entity={} spell={}",
                    _cellId, msg.sourceGuid, spellId);
            }
            break;
        }

        case MessageType::SPELL_INTERRUPT:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<SpellInterruptPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: SPELL_INTERRUPT target={} interruptedSpell={} school={} lockout={}ms",
                    _cellId, payload->targetGuid, payload->interruptedSpellId, payload->schoolMask, payload->lockoutDuration);

                WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    // Apply spell school lockout
                    if (payload->schoolMask && payload->lockoutDuration > 0)
                    {
                        target->ProhibitSpellSchool(SpellSchoolMask(payload->schoolMask), payload->lockoutDuration);
                    }

                    // Interrupt current spells
                    for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_AUTOREPEAT_SPELL; ++i)
                    {
                        if (Spell* spell = target->GetCurrentSpell(CurrentSpellTypes(i)))
                        {
                            if (spell->m_spellInfo->Id == payload->interruptedSpellId)
                            {
                                target->InterruptSpell(CurrentSpellTypes(i), false);
                                break;
                            }
                        }
                    }
                }
            }
            break;
        }

        case MessageType::SPELL_DISPEL:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<SpellDispelPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: SPELL_DISPEL target={} dispelSpell={} count={}",
                    _cellId, payload->targetGuid, payload->dispelSpellId, payload->dispelList.size());

                WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;
                WorldObject* casterObj = FindEntityByGuid(payload->casterGuid);
                Unit* caster = casterObj ? casterObj->ToUnit() : nullptr;

                if (target && target->IsInWorld())
                {
                    for (const auto& [spellId, charges] : payload->dispelList)
                    {
                        target->RemoveAurasDueToSpellByDispel(spellId, payload->dispelSpellId,
                            ObjectGuid::Empty, caster, charges);
                    }
                }
            }
            break;
        }

        case MessageType::POWER_DRAIN:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<PowerDrainPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: POWER_DRAIN target={} powerType={} amount={} isPowerBurn={}",
                    _cellId, payload->targetGuid, payload->powerType, payload->amount, payload->isPowerBurn);

                WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    Powers powerType = Powers(payload->powerType);
                    if (target->HasActivePowerType(powerType))
                    {
                        // Apply the power drain to target
                        target->ModifyPower(powerType, -payload->amount);
                    }
                }
            }
            break;
        }

        case MessageType::SPELLSTEAL:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<SpellstealPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: SPELLSTEAL target={} count={}",
                    _cellId, payload->targetGuid, payload->stealList.size());

                WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsInWorld())
                {
                    // Collect stolen aura data for cross-cell application
                    auto applyPayload = std::make_shared<SpellstealApplyPayload>();
                    applyPayload->stealerGuid = payload->casterGuid;
                    applyPayload->targetGuid = payload->targetGuid;
                    applyPayload->spellstealSpellId = payload->spellstealSpellId;

                    for (const auto& [spellId, originalCasterGuid] : payload->stealList)
                    {
                        ObjectGuid auraCasterGuid = ObjectGuid(originalCasterGuid);

                        // Find the aura to capture its data before removal
                        if (Aura* aura = target->GetAura(spellId, auraCasterGuid))
                        {
                            StolenAuraData auraData;
                            auraData.spellId = spellId;
                            auraData.originalCasterGuid = originalCasterGuid;
                            auraData.duration = aura->GetDuration();
                            auraData.maxDuration = aura->GetMaxDuration();
                            auraData.stackAmount = aura->GetStackAmount();
                            auraData.charges = aura->GetCharges();

                            // Capture base amounts from each effect
                            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                            {
                                if (AuraEffect* effect = aura->GetEffect(i))
                                    auraData.baseAmount[i] = effect->GetBaseAmount();
                            }

                            applyPayload->stolenAuras.push_back(auraData);

                            // Remove the aura from target (without applying to stealer)
                            target->RemoveAura(aura);
                        }
                    }

                    // Send stolen aura data to caster's cell for application
                    if (!applyPayload->stolenAuras.empty() && _map)
                    {
                        auto* cellMgr = _map->GetCellActorManager();
                        if (cellMgr)
                        {
                            uint32_t casterCellId = msg.sourceCellId;
                            ActorMessage applyMsg{};
                            applyMsg.type = MessageType::SPELLSTEAL_APPLY;
                            applyMsg.sourceGuid = payload->targetGuid;
                            applyMsg.targetGuid = payload->casterGuid;
                            applyMsg.sourceCellId = _cellId;
                            applyMsg.targetCellId = casterCellId;
                            applyMsg.complexPayload = applyPayload;

                            LOG_DEBUG("server.ghost", "CellActor[{}]: Sending SPELLSTEAL_APPLY to cell {} with {} auras",
                                _cellId, casterCellId, applyPayload->stolenAuras.size());

                            cellMgr->SendMessage(casterCellId, std::move(applyMsg));
                        }
                    }
                }
            }
            break;
        }

        case MessageType::SPELLSTEAL_APPLY:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<SpellstealApplyPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: SPELLSTEAL_APPLY stealer={} auras={}",
                    _cellId, payload->stealerGuid, payload->stolenAuras.size());

                WorldObject* stealerObj = FindEntityByGuid(payload->stealerGuid);
                Unit* stealer = stealerObj ? stealerObj->ToUnit() : nullptr;

                if (stealer && stealer->IsAlive() && stealer->IsInWorld())
                {
                    for (const auto& auraData : payload->stolenAuras)
                    {
                        // Apply the stolen aura to the stealer
                        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(auraData.spellId);
                        if (!spellInfo)
                            continue;

                        // Cap duration for spellsteal (2 minutes max per spell)
                        int32 dur = std::min(auraData.duration, 2 * MINUTE * IN_MILLISECONDS);

                        // Create and apply the aura with the captured data
                        if (Aura* newAura = Aura::TryCreate(spellInfo, MAX_EFFECT_MASK, stealer, stealer, nullptr))
                        {
                            // Set the stolen aura's properties
                            newAura->SetMaxDuration(dur);
                            newAura->SetDuration(dur);

                            if (auraData.stackAmount > 1)
                                newAura->SetStackAmount(auraData.stackAmount);
                            if (auraData.charges > 0)
                                newAura->SetCharges(auraData.charges);

                            // Apply effect base amounts
                            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                            {
                                if (AuraEffect* effect = newAura->GetEffect(i))
                                {
                                    effect->SetAmount(effect->CalculateAmount(stealer));
                                    // Optionally could use: effect->ChangeAmount(auraData.baseAmount[i], false);
                                }
                            }

                            newAura->ApplyForTargets();

                            LOG_DEBUG("server.ghost", "CellActor[{}]: Applied stolen aura {} to stealer {} with duration {}ms",
                                _cellId, auraData.spellId, payload->stealerGuid, dur);
                        }
                    }
                }
            }
            break;
        }

        case MessageType::POWER_CHANGED:
        {
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                // Power type in intParam1, value in intParam2, max in intParam3
                it->second->SyncPower(static_cast<uint8_t>(msg.intParam1),
                    static_cast<uint32_t>(msg.intParam2),
                    static_cast<uint32_t>(msg.intParam3));
            }
            break;
        }

        case MessageType::AURA_STATE_SYNC:
        {
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncAuraState(static_cast<uint32_t>(msg.intParam1));
            }
            break;
        }

        case MessageType::PHASE_CHANGED:
        {
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncPhaseMask(static_cast<uint32_t>(msg.intParam1));
            }
            break;
        }

        case MessageType::CONTROL_STATE_CHANGED:
        {
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                uint32_t state = static_cast<uint32_t>(msg.intParam1);
                bool apply = msg.intParam2 != 0;
                it->second->SyncControlState(state, apply);
                LOG_DEBUG("ghostactor", "CellActor[{}]: CONTROL_STATE_CHANGED ghost={} state={} apply={}",
                    _cellId, msg.sourceGuid, state, apply);
            }
            break;
        }

        case MessageType::MIGRATION_REQUEST:
        case MessageType::MIGRATION_ACK:
        case MessageType::MIGRATION_COMPLETE:
        case MessageType::MIGRATION_FORWARD:
            break;

        case MessageType::THREAT_UPDATE:
        {
            // Handle cross-cell threat modification
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<ThreatUpdatePayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: THREAT_UPDATE attacker={} victim={} delta={:.1f} new={} remove={}",
                    _cellId, payload->attackerGuid, payload->victimGuid,
                    payload->threatDelta, payload->isNewThreat, payload->isRemoval);

                // Find the attacker (creature) in this cell - threat list is on the creature
                WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
                if (!attackerObj)
                {
                    LOG_DEBUG("server.ghost", "CellActor[{}]: THREAT_UPDATE - attacker {} not found in this cell",
                        _cellId, payload->attackerGuid);
                    break;
                }

                Creature* creature = attackerObj->ToCreature();
                if (!creature)
                {
                    LOG_DEBUG("server.ghost", "CellActor[{}]: THREAT_UPDATE - attacker {} is not a creature",
                        _cellId, payload->attackerGuid);
                    break;
                }

                // We need to find the victim to apply threat
                // First check local entities
                WorldObject* victimObj = FindEntityByGuid(payload->victimGuid);
                Unit* victim = victimObj ? victimObj->ToUnit() : nullptr;

                // If victim not in this cell, try to get it via map
                if (!victim && _map)
                {
                    victim = ObjectAccessor::GetUnit(*creature, ObjectGuid(payload->victimGuid));
                }

                if (victim)
                {
                    if (payload->percentModify != 0)
                    {
                        creature->GetThreatMgr().ModifyThreatByPercent(victim, payload->percentModify);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Modified threat by {}% for victim {} on creature {}",
                            _cellId, payload->percentModify, payload->victimGuid, payload->attackerGuid);
                    }
                    else if (payload->isRemoval)
                    {
                        creature->GetThreatMgr().ModifyThreatByPercent(victim, -100);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Removed threat for victim {} from creature {}",
                            _cellId, payload->victimGuid, payload->attackerGuid);
                    }
                    else
                    {
                        creature->GetThreatMgr().AddThreat(victim, payload->threatDelta);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Added {:.1f} threat for victim {} to creature {}",
                            _cellId, payload->threatDelta, payload->victimGuid, payload->attackerGuid);
                    }
                }
            }
            break;
        }

        case MessageType::AGGRO_REQUEST:
        {
            // Handle zone-in-combat request from another cell
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<AggroRequestPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: AGGRO_REQUEST from creature={} at ({:.1f},{:.1f},{:.1f}) range={:.1f}",
                    _cellId, payload->creatureGuid,
                    payload->creatureX, payload->creatureY, payload->creatureZ,
                    payload->maxRange);

                float rangeSq = payload->maxRange * payload->maxRange;

                // Iterate players/units in this cell
                for (WorldObject* entity : _entities)
                {
                    if (!entity || !entity->IsInWorld())
                        continue;

                    Player* player = entity->ToPlayer();
                    if (!player || !player->IsAlive())
                        continue;

                    if (!(player->GetPhaseMask() & payload->creaturePhaseMask))
                        continue;

                    // Check distance
                    float dx = player->GetPositionX() - payload->creatureX;
                    float dy = player->GetPositionY() - payload->creatureY;
                    float dz = player->GetPositionZ() - payload->creatureZ;
                    float distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq <= rangeSq)
                    {
                        // Send COMBAT_INITIATED back to creature's cell
                        auto responsePayload = std::make_shared<CombatInitiatedPayload>();
                        responsePayload->entityGuid = player->GetGUID().GetRawValue();
                        responsePayload->attackerGuid = payload->creatureGuid;
                        responsePayload->threatAmount = payload->initialThreat;

                        ActorMessage responseMsg;
                        responseMsg.type = MessageType::COMBAT_INITIATED;
                        responseMsg.sourceGuid = player->GetGUID().GetRawValue();
                        responseMsg.targetGuid = payload->creatureGuid;
                        responseMsg.sourceCellId = _cellId;
                        responseMsg.targetCellId = payload->creatureCellId;
                        responseMsg.complexPayload = responsePayload;

                        // Route via map's CellActorManager
                        if (_map && _map->GetCellActorManager())
                        {
                            _map->GetCellActorManager()->SendMessage(payload->creatureCellId, std::move(responseMsg));
                        }

                        LOG_DEBUG("server.ghost", "CellActor[{}]: Player {} within range, sending COMBAT_INITIATED to cell {}",
                            _cellId, player->GetGUID().GetRawValue(), payload->creatureCellId);
                    }
                }
            }
            break;
        }

        case MessageType::COMBAT_INITIATED:
        {
            // Response from AGGRO_REQUEST - entity entered combat
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<CombatInitiatedPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: COMBAT_INITIATED entity={} entered combat with attacker={} threat={:.1f}",
                    _cellId, payload->entityGuid, payload->attackerGuid, payload->threatAmount);

                // Find the creature (attacker) in this cell
                WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
                if (!attackerObj)
                {
                    LOG_DEBUG("server.ghost", "CellActor[{}]: COMBAT_INITIATED - creature {} not found in this cell",
                        _cellId, payload->attackerGuid);
                    break;
                }

                Creature* creature = attackerObj->ToCreature();
                if (!creature || !creature->IsAlive())
                    break;

                // Find the entity (player) that entered combat
                Unit* entity = ObjectAccessor::GetUnit(*creature, ObjectGuid(payload->entityGuid));
                if (!entity || !entity->IsAlive())
                    break;

                // Set creature in combat and add threat
                creature->SetInCombatWith(entity);
                creature->GetThreatMgr().AddThreat(entity, payload->threatAmount);

                LOG_DEBUG("server.ghost", "CellActor[{}]: Creature {} now in combat with entity {}, initial threat {:.1f}",
                    _cellId, payload->attackerGuid, payload->entityGuid, payload->threatAmount);
            }
            break;
        }

        case MessageType::ASSISTANCE_REQUEST:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<AssistanceRequestPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: ASSISTANCE_REQUEST from caller={} target={} at ({:.1f},{:.1f},{:.1f}) radius={:.1f}",
                    _cellId, payload->callerGuid, payload->targetGuid,
                    payload->callerX, payload->callerY, payload->callerZ,
                    payload->radius);

                float radiusSq = payload->radius * payload->radius;

                // Find the caller creature to use CanAssistTo check
                // First try local entities, then use ObjectAccessor with a reference object
                Creature* callerCreature = nullptr;
                WorldObject* callerObj = FindEntityByGuid(payload->callerGuid);
                if (callerObj)
                {
                    callerCreature = callerObj->ToCreature();
                }

                // Iterate creatures in this cell
                for (WorldObject* entity : _entities)
                {
                    if (!entity || !entity->IsInWorld())
                        continue;

                    Creature* assistant = entity->ToCreature();
                    if (!assistant || !assistant->IsAlive())
                        continue;

                    if (assistant->GetGUID().GetRawValue() == payload->callerGuid)
                        continue;

                    if (!(assistant->GetPhaseMask() & payload->callerPhaseMask))
                        continue;

                    // Check distance from caller position
                    float dx = assistant->GetPositionX() - payload->callerX;
                    float dy = assistant->GetPositionY() - payload->callerY;
                    float dz = assistant->GetPositionZ() - payload->callerZ;
                    float distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq > radiusSq)
                        continue;

                    // Find the target to attack using this assistant as reference
                    Unit* target = ObjectAccessor::GetUnit(*assistant, ObjectGuid(payload->targetGuid));
                    if (!target || !target->IsAlive())
                        continue;

                    // Check if can assist (faction, etc) - requires caller creature
                    if (callerCreature && !assistant->CanAssistTo(callerCreature, target))
                        continue;

                    // Start combat - similar to original CallAssistance flow
                    assistant->SetNoCallAssistance(true);
                    if (assistant->IsAIEnabled && assistant->AI())
                    {
                        assistant->AI()->AttackStart(target);
                    }

                    LOG_DEBUG("server.ghost", "CellActor[{}]: Creature {} assisting caller {} against target {}",
                        _cellId, assistant->GetGUID().GetRawValue(), payload->callerGuid, payload->targetGuid);
                }
            }
            break;
        }

        case MessageType::TARGET_SWITCH:
        {
            // AI changed target - update ghost tracking
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<TargetSwitchPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: TARGET_SWITCH creature={} oldTarget={} newTarget={}",
                    _cellId, payload->creatureGuid, payload->oldTargetGuid, payload->newTargetGuid);

                // Update ghost's target info if we have a ghost for this creature
                auto it = _ghosts.find(payload->creatureGuid);
                if (it != _ghosts.end())
                {
                    it->second->SyncTargetGuid(payload->newTargetGuid);
                }
            }
            break;
        }

        case MessageType::EVADE_TRIGGERED:
        {
            LOG_DEBUG("server.ghost", "CellActor[{}]: EVADE_TRIGGERED creature={}",
                _cellId, msg.sourceGuid);

            // Update ghost's combat state to not-in-combat
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncCombatState(false);
            }
            break;
        }

        case MessageType::PET_REMOVAL:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<PetRemovalPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: PET_REMOVAL pet={} owner={} mode={}",
                    _cellId, payload->petGuid, payload->ownerGuid, payload->saveMode);

                // Find the owner player (should be in this cell)
                Player* owner = nullptr;
                for (WorldObject* entity : _entities)
                {
                    if (entity && entity->GetGUID().GetRawValue() == payload->ownerGuid)
                    {
                        owner = entity->ToPlayer();
                        break;
                    }
                }

                if (!owner)
                {
                    // Owner not in this cell - try ObjectAccessor
                    owner = ObjectAccessor::FindPlayer(ObjectGuid(payload->ownerGuid));
                }

                if (owner)
                {
                    // Find the pet via owner
                    Pet* pet = owner->GetPet();
                    if (pet && pet->GetGUID().GetRawValue() == payload->petGuid)
                    {
                        // Now safe to remove - parallel updates finished
                        owner->RemovePet(pet, static_cast<PetSaveMode>(payload->saveMode), payload->returnReagent);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Pet {} removed from owner {}",
                            _cellId, payload->petGuid, payload->ownerGuid);
                    }
                }
            }
            break;
        }

        // =====================================================================
        // Control Effects
        // TODO: Implement proper control effect application
        // These should:
        // 1. Find the target entity in this cell
        // 2. Apply the control state via Unit::SetControlled() or appropriate method
        // 3. Update all ghost projections to reflect the new state
        // =====================================================================
        case MessageType::STUN:
        {
            auto* payload = static_cast<ControlEffectPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "STUN: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: STUN caster={} target={} spell={} duration={}ms",
                _cellId, payload->casterGuid, payload->targetGuid, payload->spellId, payload->duration);

            WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
            Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

            if (!target || !target->IsAlive() || !target->IsInWorld())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: STUN - target {} not found or invalid",
                    _cellId, payload->targetGuid);
                break;
            }

            // Apply stun state - this handles movement stop, combat state, etc.
            target->SetControlled(true, UNIT_STATE_STUNNED);

            // Broadcast state change to ghost projections
            BroadcastHealthChange(target);

            LOG_DEBUG("ghostactor", "CellActor[{}]: STUN applied to {} for spell {}",
                _cellId, payload->targetGuid, payload->spellId);
            break;
        }

        case MessageType::ROOT:
        {
            auto* payload = static_cast<ControlEffectPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "ROOT: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: ROOT caster={} target={} spell={} duration={}ms",
                _cellId, payload->casterGuid, payload->targetGuid, payload->spellId, payload->duration);

            WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
            Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

            if (!target || !target->IsAlive() || !target->IsInWorld())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: ROOT - target {} not found or invalid",
                    _cellId, payload->targetGuid);
                break;
            }

            // Apply root state - target can still cast spells
            target->SetControlled(true, UNIT_STATE_ROOT);

            // Broadcast state change to ghost projections
            BroadcastHealthChange(target);

            LOG_DEBUG("ghostactor", "CellActor[{}]: ROOT applied to {} for spell {}",
                _cellId, payload->targetGuid, payload->spellId);
            break;
        }

        case MessageType::FEAR:
        {
            auto* payload = static_cast<ControlEffectPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "FEAR: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: FEAR caster={} target={} spell={} dest=({:.1f}, {:.1f}, {:.1f})",
                _cellId, payload->casterGuid, payload->targetGuid, payload->spellId,
                payload->fearDestX, payload->fearDestY, payload->fearDestZ);

            WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
            Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

            if (!target || !target->IsAlive() || !target->IsInWorld())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: FEAR - target {} not found or invalid",
                    _cellId, payload->targetGuid);
                break;
            }

            // Find the caster for fear source tracking
            Unit* caster = nullptr;
            if (payload->casterGuid)
                caster = ObjectAccessor::GetUnit(*target, ObjectGuid(payload->casterGuid));

            // Apply fear state - this triggers the flee movement generator
            // The isFear parameter distinguishes fear from other flee effects
            target->SetControlled(true, UNIT_STATE_FLEEING, caster, true);

            // Broadcast state change to ghost projections
            BroadcastHealthChange(target);

            LOG_DEBUG("ghostactor", "CellActor[{}]: FEAR applied to {} for spell {}",
                _cellId, payload->targetGuid, payload->spellId);
            break;
        }

        case MessageType::CHARM:
        {
            auto* payload = static_cast<ControlEffectPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "CHARM: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: CHARM caster={} target={} spell={} duration={}ms",
                _cellId, payload->casterGuid, payload->targetGuid, payload->spellId, payload->duration);

            WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
            Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

            if (!target || !target->IsAlive() || !target->IsInWorld())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: CHARM - target {} not found or invalid",
                    _cellId, payload->targetGuid);
                break;
            }

            // Charm is complex - faction change, threat clear, pet bar setup
            // The actual charm aura handles most of this via HandleModCharm/HandleModPossess
            // Cross-cell charm notification is mainly for AI awareness and ghost sync
            // Full implementation requires the charmer to call SetCharmedBy which
            // is normally done by the aura system, not message passing

            // Broadcast state change to ghost projections
            BroadcastHealthChange(target);

            LOG_DEBUG("ghostactor", "CellActor[{}]: CHARM notification processed for {} (actual charm via aura)",
                _cellId, payload->targetGuid);
            break;
        }

        case MessageType::KNOCKBACK:
        {
            auto* payload = static_cast<KnockbackPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "KNOCKBACK: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: KNOCKBACK target={} velocity=({:.1f}, {:.1f}) dest=({:.1f}, {:.1f}, {:.1f})",
                _cellId, payload->targetGuid, payload->speedXY, payload->speedZ,
                payload->destX, payload->destY, payload->destZ);

            WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
            Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

            if (!target || !target->IsAlive() || !target->IsInWorld())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: KNOCKBACK - target {} not found or invalid",
                    _cellId, payload->targetGuid);
                break;
            }

            // Apply knockback movement - this handles the trajectory
            target->KnockbackFrom(payload->originX, payload->originY, payload->speedXY, payload->speedZ);

            // Broadcast state change to ghost projections
            BroadcastHealthChange(target);

            LOG_DEBUG("ghostactor", "CellActor[{}]: KNOCKBACK applied to {}",
                _cellId, payload->targetGuid);
            break;
        }

        case MessageType::SILENCE:
        {
            auto* payload = static_cast<ControlEffectPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "SILENCE: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: SILENCE caster={} target={} spell={} duration={}ms",
                _cellId, payload->casterGuid, payload->targetGuid, payload->spellId, payload->duration);

            WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
            Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

            if (!target || !target->IsAlive() || !target->IsInWorld())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: SILENCE - target {} not found or invalid",
                    _cellId, payload->targetGuid);
                break;
            }

            // Apply silence flag - prevents casting with SPELL_PREVENTION_TYPE_SILENCE
            target->SetUnitFlag(UNIT_FLAG_SILENCED);

            // Interrupt any current cast that's affected by silence
            for (uint32 i = CURRENT_MELEE_SPELL; i < CURRENT_MAX_SPELL; ++i)
            {
                if (Spell* spell = target->GetCurrentSpell(CurrentSpellTypes(i)))
                {
                    if (spell->m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
                        target->InterruptSpell(CurrentSpellTypes(i), false);
                }
            }

            // Broadcast state change to ghost projections
            BroadcastHealthChange(target);

            LOG_DEBUG("ghostactor", "CellActor[{}]: SILENCE applied to {} for spell {}",
                _cellId, payload->targetGuid, payload->spellId);
            break;
        }

        case MessageType::POLYMORPH:
        {
            auto* payload = static_cast<ControlEffectPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "POLYMORPH: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: POLYMORPH caster={} target={} spell={} displayId={}",
                _cellId, payload->casterGuid, payload->targetGuid, payload->spellId, payload->transformDisplayId);

            WorldObject* targetObj = FindEntityByGuid(payload->targetGuid);
            Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

            if (!target || !target->IsAlive() || !target->IsInWorld())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: POLYMORPH - target {} not found or invalid",
                    _cellId, payload->targetGuid);
                break;
            }

            // Polymorph combines stun/incapacitate with visual transform
            // Apply stun state first (polymorph is an incapacitate)
            target->SetControlled(true, UNIT_STATE_STUNNED);

            // Apply transform display if specified
            if (payload->transformDisplayId != 0)
                target->SetDisplayId(payload->transformDisplayId);

            // Broadcast state change to ghost projections
            BroadcastHealthChange(target);

            LOG_DEBUG("ghostactor", "CellActor[{}]: POLYMORPH applied to {} displayId={}",
                _cellId, payload->targetGuid, payload->transformDisplayId);
            break;
        }

        // =====================================================================
        // Combat Feedback
        // These messages notify about attack results for procs and AI reactions
        // =====================================================================
        case MessageType::DODGE:
        {
            auto* payload = static_cast<CombatResultPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "DODGE: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: DODGE attacker={} victim={} spell={}",
                _cellId, payload->attackerGuid, payload->victimGuid, payload->spellId);

            // Find the attacker - they need to know for procs like Overpower
            WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
            Unit* attacker = attackerObj ? attackerObj->ToUnit() : nullptr;

            if (attacker && attacker->IsAlive() && attacker->IsInWorld())
            {
                // Notify creature AI that their attack was dodged
                if (Creature* creature = attacker->ToCreature())
                {
                    if (creature->IsAIEnabled && creature->AI())
                        creature->AI()->AttackDodged(ObjectGuid(payload->victimGuid));
                }

                LOG_DEBUG("ghostactor", "CellActor[{}]: DODGE notified attacker {}",
                    _cellId, payload->attackerGuid);
            }
            break;
        }

        case MessageType::PARRY:
        {
            auto* payload = static_cast<CombatResultPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "PARRY: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: PARRY attacker={} victim={} spell={}",
                _cellId, payload->attackerGuid, payload->victimGuid, payload->spellId);

            // Find attacker for parry notification
            WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
            Unit* attacker = attackerObj ? attackerObj->ToUnit() : nullptr;

            if (attacker && attacker->IsAlive() && attacker->IsInWorld())
            {
                // Notify creature AI that their attack was parried
                if (Creature* creature = attacker->ToCreature())
                {
                    if (creature->IsAIEnabled && creature->AI())
                        creature->AI()->AttackParried(ObjectGuid(payload->victimGuid));
                }

                LOG_DEBUG("ghostactor", "CellActor[{}]: PARRY notified attacker {}",
                    _cellId, payload->attackerGuid);
            }

            // Find victim for parry-haste (creatures attack faster after parrying)
            WorldObject* victimObj = FindEntityByGuid(payload->victimGuid);
            Unit* victim = victimObj ? victimObj->ToUnit() : nullptr;

            if (victim && victim->IsAlive() && victim->IsInWorld())
            {
                // Parry haste is handled in DealMeleeDamage normally
                // For cross-cell, we just notify - the actual haste was applied locally
                LOG_DEBUG("ghostactor", "CellActor[{}]: PARRY victim {} notified",
                    _cellId, payload->victimGuid);
            }
            break;
        }

        case MessageType::BLOCK:
        {
            auto* payload = static_cast<CombatResultPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "BLOCK: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: BLOCK attacker={} victim={} blocked={}",
                _cellId, payload->attackerGuid, payload->victimGuid, payload->blockedAmount);

            // Find attacker for block notification
            WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
            Unit* attacker = attackerObj ? attackerObj->ToUnit() : nullptr;

            if (attacker && attacker->IsAlive() && attacker->IsInWorld())
            {
                // Notify creature AI that their attack was blocked
                if (Creature* creature = attacker->ToCreature())
                {
                    if (creature->IsAIEnabled && creature->AI())
                        creature->AI()->AttackBlocked(ObjectGuid(payload->victimGuid), payload->blockedAmount);
                }

                LOG_DEBUG("ghostactor", "CellActor[{}]: BLOCK notified attacker {} (blocked {})",
                    _cellId, payload->attackerGuid, payload->blockedAmount);
            }
            break;
        }

        case MessageType::MISS:
        {
            auto* payload = static_cast<CombatResultPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "MISS: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: MISS attacker={} victim={} spell={}",
                _cellId, payload->attackerGuid, payload->victimGuid, payload->spellId);

            // Find attacker for miss notification
            WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
            Unit* attacker = attackerObj ? attackerObj->ToUnit() : nullptr;

            if (attacker && attacker->IsAlive() && attacker->IsInWorld())
            {
                // Notify creature AI that their attack missed
                if (Creature* creature = attacker->ToCreature())
                {
                    if (creature->IsAIEnabled && creature->AI())
                        creature->AI()->AttackMissed(ObjectGuid(payload->victimGuid));
                }

                LOG_DEBUG("ghostactor", "CellActor[{}]: MISS notified attacker {}",
                    _cellId, payload->attackerGuid);
            }
            break;
        }

        case MessageType::IMMUNE:
        {
            auto* payload = static_cast<CombatResultPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "IMMUNE: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: IMMUNE attacker={} victim={} spell={}",
                _cellId, payload->attackerGuid, payload->victimGuid, payload->spellId);

            // Notify attacker that target was immune
            WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
            Unit* attacker = attackerObj ? attackerObj->ToUnit() : nullptr;

            if (attacker && attacker->IsAlive() && attacker->IsInWorld())
            {
                // Notify creature AI about immunity
                if (Creature* creature = attacker->ToCreature())
                {
                    if (creature->IsAIEnabled && creature->AI())
                        creature->AI()->SpellImmune(ObjectGuid(payload->victimGuid), payload->spellId);
                }

                LOG_DEBUG("ghostactor", "CellActor[{}]: IMMUNE notified attacker {} for spell {}",
                    _cellId, payload->attackerGuid, payload->spellId);
            }
            break;
        }

        case MessageType::ABSORB_NOTIFICATION:
        {
            auto* payload = static_cast<CombatResultPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "ABSORB: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: ABSORB attacker={} victim={} absorbed={}",
                _cellId, payload->attackerGuid, payload->victimGuid, payload->absorbedAmount);

            // Notify attacker that damage was absorbed
            WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
            Unit* attacker = attackerObj ? attackerObj->ToUnit() : nullptr;

            if (attacker && attacker->IsAlive() && attacker->IsInWorld())
            {
                // Notify creature AI about absorbed damage
                if (Creature* creature = attacker->ToCreature())
                {
                    if (creature->IsAIEnabled && creature->AI())
                        creature->AI()->DamageAbsorbed(ObjectGuid(payload->victimGuid), payload->absorbedAmount);
                }

                LOG_DEBUG("ghostactor", "CellActor[{}]: ABSORB notified attacker {} (absorbed {})",
                    _cellId, payload->attackerGuid, payload->absorbedAmount);
            }
            break;
        }

        // =====================================================================
        // Spell Cast Notifications
        // TODO: Implement for AI interrupt logic and procs
        // =====================================================================
        case MessageType::SPELL_CAST_START:
        {
            auto* payload = static_cast<SpellCastPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "SPELL_CAST_START: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: SPELL_CAST_START caster={} spell={} castTime={}ms school={}",
                _cellId, payload->casterGuid, payload->spellId, payload->castTime, payload->schoolMask);

            // Notify all creature AIs in this cell about the nearby cast
            // AI creatures may decide to interrupt based on spell school, cast time, danger level
            ObjectGuid casterObjGuid(payload->casterGuid);
            for (WorldObject* obj : _entities)
            {
                if (!obj || !obj->IsCreature())
                    continue;

                Creature* creature = obj->ToCreature();
                if (!creature->IsAIEnabled || !creature->AI())
                    continue;

                // Don't notify the caster about their own cast
                if (creature->GetGUID() == casterObjGuid)
                    continue;

                creature->AI()->OnNearbyCastStarted(casterObjGuid, payload->spellId,
                    payload->castTime, payload->schoolMask);
            }
            break;
        }

        case MessageType::SPELL_CAST_FAILED:
        {
            auto* payload = static_cast<SpellCastPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "SPELL_CAST_FAILED: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: SPELL_CAST_FAILED caster={} spell={} reason={}",
                _cellId, payload->casterGuid, payload->spellId, payload->failReason);

            // Notify creature AIs that a nearby cast was interrupted/failed
            ObjectGuid casterObjGuid(payload->casterGuid);
            for (WorldObject* obj : _entities)
            {
                if (!obj || !obj->IsCreature())
                    continue;

                Creature* creature = obj->ToCreature();
                if (!creature->IsAIEnabled || !creature->AI())
                    continue;

                if (creature->GetGUID() == casterObjGuid)
                    continue;

                creature->AI()->OnNearbyCastFailed(casterObjGuid, payload->spellId, payload->failReason);
            }
            break;
        }

        case MessageType::SPELL_CAST_SUCCESS:
        {
            auto* payload = static_cast<SpellCastPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "SPELL_CAST_SUCCESS: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: SPELL_CAST_SUCCESS caster={} spell={}",
                _cellId, payload->casterGuid, payload->spellId);

            // Notify creature AIs that a nearby cast completed
            // Can be used for reactive AI behaviors or on-cast procs
            ObjectGuid casterObjGuid(payload->casterGuid);
            for (WorldObject* obj : _entities)
            {
                if (!obj || !obj->IsCreature())
                    continue;

                Creature* creature = obj->ToCreature();
                if (!creature->IsAIEnabled || !creature->AI())
                    continue;

                if (creature->GetGUID() == casterObjGuid)
                    continue;

                creature->AI()->OnNearbyCastSuccess(casterObjGuid, payload->spellId);
            }
            break;
        }

        // =====================================================================
        // Taunt Messages - Cross-cell taunt/detaunt for tank gameplay
        // =====================================================================
        case MessageType::TAUNT:
        {
            auto* payload = static_cast<TauntPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "TAUNT: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: TAUNT taunter={} creature={} spell={} duration={}ms",
                _cellId, payload->taunterGuid, payload->targetGuid, payload->spellId, payload->duration);

            // Find the creature being taunted in this cell
            WorldObject* creatureObj = FindEntityByGuid(payload->targetGuid);
            if (!creatureObj)
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: TAUNT - creature {} not found in cell",
                    _cellId, payload->targetGuid);
                break;
            }

            Creature* creature = creatureObj->ToCreature();
            if (!creature || !creature->IsAlive() || !creature->CanHaveThreatList())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: TAUNT - creature {} invalid or cannot have threat",
                    _cellId, payload->targetGuid);
                break;
            }

            // Find the taunter - may be in another cell, use ObjectAccessor
            Unit* taunter = ObjectAccessor::GetUnit(*creature, ObjectGuid(payload->taunterGuid));
            if (!taunter || !taunter->IsAlive())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: TAUNT - taunter {} not found or dead",
                    _cellId, payload->taunterGuid);
                break;
            }

            // Skip if taunter is GM
            if (taunter->IsPlayer() && taunter->ToPlayer()->IsGameMaster())
                break;

            // Skip if creature is passive
            if (creature->HasReactState(REACT_PASSIVE))
                break;

            // Apply taunt logic: raise threat to top and force target switch
            ThreatMgr& threatMgr = creature->GetThreatMgr();
            if (!threatMgr.GetOnlineContainer().empty())
            {
                // Raise taunter's threat to match top threat (like EffectTaunt)
                float myThreat = threatMgr.GetThreat(taunter);
                float topThreat = threatMgr.GetOnlineContainer().getMostHated()->GetThreat();
                if (topThreat > myThreat)
                {
                    threatMgr.DoAddThreat(taunter, topThreat - myThreat);
                }

                // Force victim switch to taunter
                if (HostileReference* forcedVictim = threatMgr.GetOnlineContainer().getReferenceByTarget(taunter))
                {
                    threatMgr.setCurrentVictim(forcedVictim);
                }
            }
            else
            {
                // No threat list - add taunter with 110% threat (TauntApply logic)
                threatMgr.AddThreat(taunter, 1.0f);
                if (HostileReference* forcedVictim = threatMgr.GetOnlineContainer().getReferenceByTarget(taunter))
                {
                    forcedVictim->SetThreat(1.1f);
                    threatMgr.setCurrentVictim(forcedVictim);
                }
            }

            // Update target field and start attack
            creature->SetGuidValue(UNIT_FIELD_TARGET, taunter->GetGUID());
            if (creature->IsAIEnabled)
            {
                creature->AI()->AttackStart(taunter);
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: TAUNT applied - creature {} now targeting taunter {}",
                _cellId, payload->targetGuid, payload->taunterGuid);
            break;
        }

        case MessageType::DETAUNT:
        {
            auto* payload = static_cast<DetauntPayload*>(msg.complexPayload.get());
            if (!payload)
            {
                LOG_DEBUG("ghostactor", "DETAUNT: missing payload");
                break;
            }

            LOG_DEBUG("ghostactor", "CellActor[{}]: DETAUNT source={} creature={} threatPct={:.1f}% removeTaunt={}",
                _cellId, payload->sourceGuid, payload->targetGuid,
                payload->threatReductionPct, payload->removeTaunt);

            // Find the creature in this cell
            WorldObject* creatureObj = FindEntityByGuid(payload->targetGuid);
            if (!creatureObj)
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: DETAUNT - creature {} not found in cell",
                    _cellId, payload->targetGuid);
                break;
            }

            Creature* creature = creatureObj->ToCreature();
            if (!creature || !creature->IsAlive() || !creature->CanHaveThreatList())
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: DETAUNT - creature {} invalid or cannot have threat",
                    _cellId, payload->targetGuid);
                break;
            }

            // Find the source unit - may be in another cell
            Unit* source = ObjectAccessor::GetUnit(*creature, ObjectGuid(payload->sourceGuid));
            if (!source)
            {
                LOG_DEBUG("ghostactor", "CellActor[{}]: DETAUNT - source {} not found",
                    _cellId, payload->sourceGuid);
                break;
            }

            ThreatMgr& threatMgr = creature->GetThreatMgr();

            // Apply threat reduction if specified
            if (payload->threatReductionPct > 0.0f)
            {
                float currentThreat = threatMgr.GetThreat(source);
                if (currentThreat > 0.0f)
                {
                    float reduction = currentThreat * (payload->threatReductionPct / 100.0f);
                    threatMgr.ModifyThreatByPercent(source, static_cast<int32_t>(-payload->threatReductionPct));
                    LOG_DEBUG("ghostactor", "CellActor[{}]: DETAUNT - reduced threat by {:.1f}% ({:.1f} -> {:.1f})",
                        _cellId, payload->threatReductionPct, currentThreat, threatMgr.GetThreat(source));
                }
            }

            // Handle taunt fadeout - similar to TauntFadeOut logic
            if (payload->removeTaunt)
            {
                Unit* currentVictim = creature->GetVictim();
                if (currentVictim && currentVictim == source)
                {
                    // Current victim was the detaunter - select new victim
                    if (threatMgr.isThreatListEmpty())
                    {
                        if (creature->IsAIEnabled)
                            creature->AI()->EnterEvadeMode(CreatureAI::EVADE_REASON_NO_HOSTILES);
                    }
                    else
                    {
                        Unit* newVictim = creature->SelectVictim();
                        if (newVictim && newVictim != source)
                        {
                            creature->SetGuidValue(UNIT_FIELD_TARGET, newVictim->GetGUID());
                            creature->SetInFront(newVictim);
                            if (creature->IsAIEnabled)
                                creature->AI()->AttackStart(newVictim);

                            LOG_DEBUG("ghostactor", "CellActor[{}]: DETAUNT - creature {} switched target to {}",
                                _cellId, payload->targetGuid, newVictim->GetGUID().GetRawValue());
                        }
                    }
                }
            }
            break;
        }

        // =====================================================================
        // Resurrection Messages
        // TODO: Implement for cross-cell resurrection
        // =====================================================================
        case MessageType::RESURRECT_REQUEST:
        {
            // TODO: Handle resurrection request
            // - Find dead player by targetGuid
            // - Store resurrection offer (health%, mana%, location)
            // - Show accept/decline dialog to player
            [[maybe_unused]] auto* payload = static_cast<ResurrectPayload*>(msg.complexPayload.get());
            LOG_DEBUG("ghostactor", "RESURRECT_REQUEST: {} offering to resurrect {} [NOT IMPLEMENTED]",
                msg.sourceGuid, msg.targetGuid);
            break;
        }

        case MessageType::RESURRECT_ACCEPT:
        {
            // TODO: Handle resurrection acceptance
            // - Find the original caster
            // - Complete the resurrection spell
            // - Player respawns at designated location
            LOG_DEBUG("ghostactor", "RESURRECT_ACCEPT: {} accepting resurrection from {} [NOT IMPLEMENTED]",
                msg.sourceGuid, msg.targetGuid);
            break;
        }

        // =====================================================================
        // Player Social Messages
        // TODO: Implement for cross-cell duels and trades
        // =====================================================================
        case MessageType::DUEL_REQUEST:
        {
            // TODO: Handle duel challenge
            // - Find target player
            // - Send duel request packet
            // - Spawn duel flag gameobject
            [[maybe_unused]] auto* payload = static_cast<DuelPayload*>(msg.complexPayload.get());
            LOG_DEBUG("ghostactor", "DUEL_REQUEST: {} challenging {} [NOT IMPLEMENTED]",
                msg.sourceGuid, msg.targetGuid);
            break;
        }

        case MessageType::DUEL_STARTED:
        {
            // TODO: Handle duel start
            // - Both players enter duel combat state
            // - Timer and boundaries active
            LOG_DEBUG("ghostactor", "DUEL_STARTED: {} vs {} [NOT IMPLEMENTED]",
                msg.sourceGuid, msg.targetGuid);
            break;
        }

        case MessageType::DUEL_ENDED:
        {
            // TODO: Handle duel end
            // - Determine winner/loser/fled
            // - Clean up duel state
            // - Remove duel flag
            auto* payload = static_cast<DuelPayload*>(msg.complexPayload.get());
            LOG_DEBUG("ghostactor", "DUEL_ENDED: result {} [NOT IMPLEMENTED]",
                payload ? payload->result : 0);
            break;
        }

        case MessageType::TRADE_REQUEST:
        {
            // TODO: Handle trade request
            // - Find target player
            // - Send trade request packet
            [[maybe_unused]] auto* payload = static_cast<TradePayload*>(msg.complexPayload.get());
            LOG_DEBUG("ghostactor", "TRADE_REQUEST: {} requesting trade with {} [NOT IMPLEMENTED]",
                msg.sourceGuid, msg.targetGuid);
            break;
        }

        default:
            break;
    }
}

void CellActor::UpdateEntities(uint32_t diff)
{
    bool parallelUpdatesEnabled = sWorld->getBoolConfig(CONFIG_PARALLEL_UPDATES_ENABLED);

    _isUpdating = true;

    for (WorldObject* entity : _entities)
    {
        // Check pending removals FIRST - the memory may already be freed!
        // Must do this before accessing any entity members.
        if (!entity || std::find(_pendingRemovals.begin(), _pendingRemovals.end(), entity) != _pendingRemovals.end())
            continue;

        if (!entity->IsInWorld())
            continue;

        if (GameObject* go = entity->ToGameObject())
        {
            go->Update(diff);
        }
        else if (Creature* creature = entity->ToCreature())
        {
            if (creature->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
                continue;

            if (parallelUpdatesEnabled)
                creature->UpdateParallel(diff);
            else
            {
                creature->UpdateRegeneration(diff);
                creature->UpdateTimersParallel(diff);
            }
        }
        else if (Player* player = entity->ToPlayer())
        {
            if (player->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
                continue;

            if (parallelUpdatesEnabled)
                player->UpdateParallel(diff);
        }
    }

    _isUpdating = false;
    ProcessPendingRemovals();
}

void CellActor::AddEntity(WorldObject* obj)
{
    if (!obj)
        return;

    _entities.push_back(obj);
    _isActive.store(true, std::memory_order_release);  // Mark cell as active
}

void CellActor::RemoveEntity(WorldObject* obj)
{
    if (!obj)
        return;

    // Defer removal if we're currently iterating
    if (_isUpdating)
    {
        _pendingRemovals.push_back(obj);
        return;
    }

    auto it = std::find(_entities.begin(), _entities.end(), obj);
    if (it != _entities.end())
    {
        // Swap with last and pop for O(1) removal
        std::swap(*it, _entities.back());
        _entities.pop_back();

        // Clear active flag when no entities remain
        if (_entities.empty())
            _isActive.store(false, std::memory_order_release);
    }
}

void CellActor::ProcessPendingRemovals()
{
    for (WorldObject* obj : _pendingRemovals)
    {
        auto it = std::find(_entities.begin(), _entities.end(), obj);
        if (it != _entities.end())
        {
            std::swap(*it, _entities.back());
            _entities.pop_back();
        }
    }
    _pendingRemovals.clear();

    if (_entities.empty())
        _isActive.store(false, std::memory_order_release);
}

WorldObject* CellActor::FindEntityByGuid(uint64_t guid) const
{
    for (WorldObject* entity : _entities)
    {
        if (entity && entity->GetGUID().GetRawValue() == guid)
            return entity;
    }
    return nullptr;
}

void CellActor::BroadcastHealthChange(Unit* unit)
{
    if (!unit || !_map)
        return;

    auto* cellMgr = _map->GetCellActorManager();
    if (!cellMgr)
        return;

    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = unit->GetGUID().GetRawValue();
    msg.sourceCellId = _cellId;
    msg.intParam1 = static_cast<int64_t>(unit->GetHealth());
    msg.intParam2 = static_cast<int64_t>(unit->GetMaxHealth());

    // Send to all neighboring cells that might have ghosts of this entity
    std::vector<uint32_t> neighbors = cellMgr->GetNeighborCellIds(_cellId);
    for (uint32_t neighborCellId : neighbors)
    {
        cellMgr->SendMessage(neighborCellId, msg);
    }
}

// ============================================================================
// GhostEntity Implementation
// ============================================================================

GhostEntity::GhostEntity(uint64_t guid, uint32_t ownerCellId)
    : _guid(guid)
    , _ownerCellId(ownerCellId)
{
}

void GhostEntity::SyncFromSnapshot(const GhostSnapshot& snapshot)
{
    _posX = snapshot.posX;
    _posY = snapshot.posY;
    _posZ = snapshot.posZ;
    _orientation = snapshot.orientation;
    _health = snapshot.health;
    _maxHealth = snapshot.maxHealth;
    _displayId = snapshot.displayId;
    _moveFlags = snapshot.moveFlags;
    _inCombat = snapshot.inCombat;
    _isDead = snapshot.isDead;
}

void GhostEntity::SyncPosition(float x, float y, float z, float o)
{
    _posX = x;
    _posY = y;
    _posZ = z;
    _orientation = o;
}

void GhostEntity::SyncHealth(uint32_t health, uint32_t maxHealth)
{
    _health = health;
    _maxHealth = maxHealth;
    _isDead = (health == 0);
}

void GhostEntity::SyncCombatState(bool inCombat)
{
    _inCombat = inCombat;
}

void GhostEntity::SyncPower(uint8_t power, uint32_t value, uint32_t maxValue)
{
    if (power < 7)  // MAX_POWERS
    {
        _power[power] = value;
        _maxPower[power] = maxValue;
    }
}

uint32_t GhostEntity::GetPower(uint8_t power) const
{
    if (power < 7)  // MAX_POWERS
        return _power[power];
    return 0;
}

void GhostEntity::SyncAuraApplied(uint32_t spellId, uint8_t /*effectMask*/)
{
    _activeAuras.insert(spellId);

    // Update aura state flags based on spell's aura state type
    if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId))
    {
        AuraStateType auraState = spellInfo->GetAuraState();
        if (auraState != AURA_STATE_NONE)
        {
            _auraState |= (1 << (auraState - 1));
        }
    }
}

void GhostEntity::SyncAuraRemoved(uint32_t spellId)
{
    _activeAuras.erase(spellId);

    // Recalculate aura state from remaining auras
    RecalculateAuraState();
}

void GhostEntity::RecalculateAuraState()
{
    uint32_t newState = 0;

    for (uint32_t activeSpellId : _activeAuras)
    {
        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(activeSpellId))
        {
            AuraStateType auraState = spellInfo->GetAuraState();
            if (auraState != AURA_STATE_NONE)
            {
                newState |= (1 << (auraState - 1));
            }
        }
    }

    _auraState = newState;
}

// ============================================================================
// CellActorManager Implementation
// ============================================================================

CellActorManager::CellActorManager(Map* map)
    : _map(map)
    , _cellActors(new CellActor*[TOTAL_GRIDS])
{
    // Pre-allocate all grids for lock-free O(1) access (64x64 = 4,096)
    // Using raw pointers eliminates unique_ptr::get() overhead on hot path
    for (uint32_t y = 0; y < GRIDS_PER_DIMENSION; ++y)
    {
        for (uint32_t x = 0; x < GRIDS_PER_DIMENSION; ++x)
        {
            uint32_t index = y * GRIDS_PER_DIMENSION + x;
            uint32_t gridId = MakeGridId(x, y);
            _cellActors[index] = new CellActor(gridId, map);
        }
    }
}

CellActorManager::~CellActorManager()
{
    // Clean up all allocated grids
    for (uint32_t i = 0; i < TOTAL_GRIDS; ++i)
    {
        delete _cellActors[i];
    }
    delete[] _cellActors;
}

// GetOrCreateCellActor and GetCellActor are now inline in header for performance

CellActor* CellActorManager::GetCellActorForPosition(float x, float y)
{
    uint32_t gridId = CalculateGridId(x, y);
    return _cellActors[gridId];
}

void CellActorManager::Update(uint32_t diff)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    // Reset per-tick counters at start of update
    _perfStats.ResetTickCounters();

    if (!_workPool)
    {
        // Single-threaded path: iterate all cells, skip inactive
        for (uint32_t i = 0; i < TOTAL_GRIDS; ++i)
        {
            CellActor* cell = _cellActors[i];
            if (cell && cell->IsActive() && cell->HasWork())
                cell->Update(diff);
        }
    }
    else
    {
        // Parallel path: submit active cells to work pool
        for (uint32_t i = 0; i < TOTAL_GRIDS; ++i)
        {
            CellActor* cell = _cellActors[i];
            if (cell && cell->IsActive() && cell->HasWork())
            {
                _pendingCellUpdates.fetch_add(1, std::memory_order_release);

                _workPool->Submit(TaskType::CELL, [this, cell, diff]() {
                    cell->Update(diff);
                    _pendingCellUpdates.fetch_sub(1, std::memory_order_release);
                });
            }
        }

        while (_pendingCellUpdates.load(std::memory_order_acquire) > 0)
        {
            if (!_workPool->TryExecuteOne(TaskType::CELL))
                std::this_thread::yield();
        }
    }

    // Record update timing
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    _perfStats.RecordUpdateTime(static_cast<uint64_t>(duration.count()));
}

void CellActorManager::SendMessage(uint32_t targetCellId, ActorMessage msg)
{
    if (targetCellId < TOTAL_GRIDS && _cellActors[targetCellId])
    {
        _cellActors[targetCellId]->SendMessage(std::move(msg));
    }
}

void CellActorManager::OnEntityAdded(WorldObject* obj)
{
    if (!obj)
        return;

    CellActor* cell = GetCellActorForPosition(obj->GetPositionX(), obj->GetPositionY());
    cell->AddEntity(obj);

    if (obj->IsGameObject())
        obj->SetCellManaged(true);
    else if (obj->ToCreature() && sWorld->getBoolConfig(CONFIG_PARALLEL_UPDATES_ENABLED))
        obj->SetCellManaged(true);
    else if (obj->ToPlayer() && sWorld->getBoolConfig(CONFIG_PARALLEL_UPDATES_ENABLED))
        obj->SetCellManaged(true);
}

void CellActorManager::OnEntityRemoved(WorldObject* obj)
{
    if (!obj)
        return;

    CellActor* cell = GetCellActorForPosition(obj->GetPositionX(), obj->GetPositionY());
    if (cell)
        cell->RemoveEntity(obj);

    if (obj->IsGameObject())
        obj->SetCellManaged(false);
    else if (obj->ToCreature())
        obj->SetCellManaged(false);
    else if (obj->ToPlayer())
        obj->SetCellManaged(false);
}

void CellActorManager::OnEntityMoved(WorldObject* obj, float oldX, float oldY)
{
    if (!obj)
        return;

    // Calculate old and new grid IDs
    uint32_t oldGridId = CalculateGridId(oldX, oldY);

    float newX = obj->GetPositionX();
    float newY = obj->GetPositionY();
    uint32_t newGridId = CalculateGridId(newX, newY);

    // If grid changed, transfer entity
    if (oldGridId != newGridId)
    {
        CellActor* oldCell = _cellActors[oldGridId];
        CellActor* newCell = _cellActors[newGridId];

        if (oldCell)
        {
            oldCell->RemoveEntity(obj);

            // Send migration message
            ActorMessage msg;
            msg.type = MessageType::ENTITY_LEAVING;
            msg.sourceGuid = obj->GetGUID().GetRawValue();
            msg.sourceCellId = oldGridId;
            msg.targetCellId = newGridId;
            oldCell->SendMessage(std::move(msg));
        }

        newCell->AddEntity(obj);

        // Notify new cell of arrival
        ActorMessage enterMsg;
        enterMsg.type = MessageType::ENTITY_ENTERING;
        enterMsg.sourceGuid = obj->GetGUID().GetRawValue();
        enterMsg.sourceCellId = oldGridId;
        enterMsg.targetCellId = newGridId;
        enterMsg.floatParam1 = newX;
        enterMsg.floatParam2 = newY;
        enterMsg.floatParam3 = obj->GetPositionZ();
        newCell->SendMessage(std::move(enterMsg));
    }

    // Update ghosts after movement
    UpdateEntityGhosts(obj);
}

// ============================================================================
// Ghost Management
// ============================================================================

GhostSnapshot CellActorManager::CreateSnapshotFromEntity(WorldObject* obj)
{
    GhostSnapshot snapshot;
    if (!obj)
        return snapshot;

    snapshot.guid = obj->GetGUID().GetRawValue();
    snapshot.phaseMask = obj->GetPhaseMask();
    snapshot.posX = obj->GetPositionX();
    snapshot.posY = obj->GetPositionY();
    snapshot.posZ = obj->GetPositionZ();
    snapshot.orientation = obj->GetOrientation();

    // Get health if it's a unit
    if (obj->IsUnit())
    {
        Unit* unit = obj->ToUnit();
        snapshot.health = unit->GetHealth();
        snapshot.maxHealth = unit->GetMaxHealth();
        snapshot.inCombat = unit->IsInCombat();
        snapshot.isDead = !unit->IsAlive();
        snapshot.displayId = unit->GetDisplayId();
    }

    return snapshot;
}

void CellActorManager::UpdateEntityGhosts(WorldObject* obj)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    float x = obj->GetPositionX();
    float y = obj->GetPositionY();

    uint32_t homeCellId = CalculateGridId(x, y);

    // Determine which neighbors need ghosts
    NeighborFlags neededGhosts = GhostBoundary::GetNeighborsNeedingGhosts(x, y);

    // Get or create ghost info for this entity
    auto& info = _entityGhostInfo[guid];
    if (info.guid == 0)
    {
        info.guid = guid;
        info.homeCellId = homeCellId;
    }

    NeighborFlags currentGhosts = info.activeGhosts;

    // Array of all directions to check
    static const NeighborFlags allDirections[] = {
        NeighborFlags::NORTH, NeighborFlags::SOUTH,
        NeighborFlags::EAST, NeighborFlags::WEST,
        NeighborFlags::NORTH_EAST, NeighborFlags::NORTH_WEST,
        NeighborFlags::SOUTH_EAST, NeighborFlags::SOUTH_WEST
    };

    GhostSnapshot snapshot;
    bool snapshotCreated = false;

    for (NeighborFlags dir : allDirections)
    {
        bool needsGhost = HasFlag(neededGhosts, dir);
        bool hasGhost = HasFlag(currentGhosts, dir);

        if (needsGhost && !hasGhost)
        {
            // Create ghost in this neighbor
            if (!snapshotCreated)
            {
                snapshot = CreateSnapshotFromEntity(obj);
                snapshotCreated = true;
            }

            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);
            CreateGhostInCell(neighborCellId, snapshot, homeCellId);
            info.activeGhosts = info.activeGhosts | dir;
        }
        else if (!needsGhost && hasGhost)
        {
            // Destroy ghost in this neighbor
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);
            DestroyGhostInCell(neighborCellId, guid);
            info.activeGhosts = static_cast<NeighborFlags>(
                static_cast<uint8_t>(info.activeGhosts) & ~static_cast<uint8_t>(dir));
        }
        else if (needsGhost && hasGhost)
        {
            // Update position in existing ghost
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);

            ActorMessage msg;
            msg.type = MessageType::POSITION_UPDATE;
            msg.sourceGuid = guid;
            msg.sourceCellId = homeCellId;
            msg.floatParam1 = x;
            msg.floatParam2 = y;
            msg.floatParam3 = obj->GetPositionZ();
            UpdateGhostInCell(neighborCellId, guid, msg);
        }
    }

    info.homeCellId = homeCellId;
    if (snapshotCreated)
        info.lastSnapshot = snapshot;
}

void CellActorManager::OnEntityHealthChanged(WorldObject* obj, uint32_t health, uint32_t maxHealth)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg;
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(health);
    msg.intParam2 = static_cast<int32_t>(maxHealth);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityCombatStateChanged(WorldObject* obj, bool inCombat)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg;
    msg.type = MessageType::COMBAT_STATE_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = inCombat ? 1 : 0;

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityPowerChanged(WorldObject* obj, uint8_t power, uint32_t value, uint32_t maxValue)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::POWER_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(power);
    msg.intParam2 = static_cast<int32_t>(value);
    msg.intParam3 = static_cast<int32_t>(maxValue);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityAuraStateChanged(WorldObject* obj, uint32_t auraState)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::AURA_STATE_SYNC;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(auraState);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityAuraApplied(WorldObject* obj, uint32_t spellId, uint8_t effectMask)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::AURA_APPLY;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(spellId);
    msg.intParam2 = static_cast<int32_t>(effectMask);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityAuraRemoved(WorldObject* obj, uint32_t spellId)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::AURA_REMOVE;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(spellId);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityPhaseChanged(WorldObject* obj, uint32_t newPhaseMask)
{
    // Must be valid and in world (SetPhaseMask can be called during Create before object is ready)
    if (!obj || !obj->IsInWorld())
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    // Update the cached phase mask in ghost info
    it->second.lastSnapshot.phaseMask = newPhaseMask;

    if (it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::PHASE_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(newPhaseMask);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityControlStateChanged(WorldObject* obj, uint32_t state, bool apply)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::CONTROL_STATE_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int64_t>(state);
    msg.intParam2 = apply ? 1 : 0;

    BroadcastToGhosts(guid, msg);

    LOG_DEBUG("ghostactor", "OnEntityControlStateChanged: guid={} state={} apply={}",
        guid, state, apply);
}

bool CellActorManager::CanInteractCrossPhase(WorldObject* source, uint64_t targetGuid)
{
    if (!source)
        return false;

    // Check if target is tracked as a ghost - if so, use ghost's phase mask
    auto it = _entityGhostInfo.find(targetGuid);
    if (it != _entityGhostInfo.end())
    {
        return (source->GetPhaseMask() & it->second.lastSnapshot.phaseMask) != 0;
    }

    // Not a tracked entity, allow interaction (same-cell interactions use normal checks)
    return true;
}

void CellActorManager::BroadcastToGhosts(uint64_t guid, const ActorMessage& msg)
{
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    const EntityGhostInfo& info = it->second;
    if (info.activeGhosts == NeighborFlags::NONE)
        return;

    static const NeighborFlags allDirections[] = {
        NeighborFlags::NORTH, NeighborFlags::SOUTH,
        NeighborFlags::EAST, NeighborFlags::WEST,
        NeighborFlags::NORTH_EAST, NeighborFlags::NORTH_WEST,
        NeighborFlags::SOUTH_EAST, NeighborFlags::SOUTH_WEST
    };

    for (NeighborFlags dir : allDirections)
    {
        if (HasFlag(info.activeGhosts, dir))
        {
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(info.homeCellId, dir);
            UpdateGhostInCell(neighborCellId, guid, msg);
        }
    }
}

void CellActorManager::CreateGhostInCell(uint32_t cellId, const GhostSnapshot& snapshot, uint32_t ownerCellId)
{
    // Ensure target grid exists (ghosts need somewhere to live)
    // Grid ID is the array index directly for 64x64 grids

    ActorMessage msg;
    msg.type = MessageType::GHOST_CREATE;
    msg.sourceGuid = snapshot.guid;
    msg.sourceCellId = ownerCellId;  // Where the real entity lives
    msg.targetCellId = cellId;       // Where the ghost is being created
    msg.floatParam1 = snapshot.posX;
    msg.floatParam2 = snapshot.posY;
    msg.floatParam3 = snapshot.posZ;
    msg.intParam1 = static_cast<int32_t>(snapshot.health);
    msg.intParam2 = static_cast<int32_t>(snapshot.maxHealth);

    // Store full snapshot in complexPayload
    auto snapshotCopy = std::make_shared<GhostSnapshot>(snapshot);
    msg.complexPayload = snapshotCopy;

    SendMessage(cellId, std::move(msg));
}

void CellActorManager::UpdateGhostInCell(uint32_t cellId, uint64_t guid, const ActorMessage& msg)
{
    ActorMessage updateMsg = msg;
    updateMsg.targetCellId = cellId;
    SendMessage(cellId, std::move(updateMsg));
}

void CellActorManager::DestroyGhostInCell(uint32_t cellId, uint64_t guid)
{
    ActorMessage msg;
    msg.type = MessageType::GHOST_DESTROY;
    msg.sourceGuid = guid;
    msg.targetCellId = cellId;
    SendMessage(cellId, std::move(msg));
}

void CellActorManager::DestroyAllGhostsForEntity(uint64_t guid)
{
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    EntityGhostInfo& info = it->second;
    uint32_t homeCellId = info.homeCellId;

    // Destroy ghost in each active neighbor direction
    static const NeighborFlags directions[] = {
        NeighborFlags::NORTH, NeighborFlags::SOUTH,
        NeighborFlags::EAST, NeighborFlags::WEST,
        NeighborFlags::NORTH_EAST, NeighborFlags::NORTH_WEST,
        NeighborFlags::SOUTH_EAST, NeighborFlags::SOUTH_WEST
    };

    for (NeighborFlags dir : directions)
    {
        if (static_cast<uint8_t>(info.activeGhosts) & static_cast<uint8_t>(dir))
        {
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);
            DestroyGhostInCell(neighborCellId, guid);
        }
    }

    // Clear ghost info
    _entityGhostInfo.erase(it);
}

// ============================================================================
// Cell Migration
// ============================================================================

uint64_t CellActorManager::GenerateMigrationId()
{
    return _nextMigrationId.fetch_add(1, std::memory_order_relaxed);
}

MigrationSnapshot CellActorManager::CreateMigrationSnapshot(WorldObject* obj)
{
    MigrationSnapshot snapshot;
    if (!obj)
        return snapshot;

    snapshot.guid = obj->GetGUID().GetRawValue();
    snapshot.entry = obj->GetEntry();
    snapshot.typeId = obj->GetTypeId();
    snapshot.posX = obj->GetPositionX();
    snapshot.posY = obj->GetPositionY();
    snapshot.posZ = obj->GetPositionZ();
    snapshot.orientation = obj->GetOrientation();
    snapshot.mapId = obj->GetMapId();

    if (obj->IsUnit())
    {
        Unit* unit = obj->ToUnit();
        snapshot.health = unit->GetHealth();
        snapshot.maxHealth = unit->GetMaxHealth();
        snapshot.power = unit->GetPower(unit->getPowerType());
        snapshot.maxPower = unit->GetMaxPower(unit->getPowerType());
        snapshot.powerType = static_cast<uint8_t>(unit->getPowerType());
        snapshot.displayId = unit->GetDisplayId();
        snapshot.nativeDisplayId = unit->GetNativeDisplayId();
        snapshot.inCombat = unit->IsInCombat();
        snapshot.isDead = !unit->IsAlive();
        snapshot.targetGuid = unit->GetTarget().GetRawValue();

        if (Creature* creature = obj->ToCreature())
        {
            snapshot.reactState = static_cast<uint32_t>(creature->GetReactState());
        }
    }

    return snapshot;
}

void CellActorManager::CheckAndInitiateMigration(WorldObject* obj, float oldX, float oldY)
{
    if (!obj)
        return;

    if (Unit* unit = obj->ToUnit())
    {
        if (unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
            return;
    }

    uint64_t guid = obj->GetGUID().GetRawValue();

    if (IsEntityMigrating(guid))
        return;

    // Calculate old and new grid IDs
    uint32_t oldGridId = CalculateGridId(oldX, oldY);

    float newX = obj->GetPositionX();
    float newY = obj->GetPositionY();
    uint32_t newGridId = CalculateGridId(newX, newY);

    // Check if grid boundary was crossed
    if (oldGridId != newGridId)
    {
        InitiateMigration(obj, oldGridId, newGridId);
    }
}

void CellActorManager::InitiateMigration(WorldObject* obj, uint32_t oldCellId, uint32_t newCellId)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    uint64_t migrationId = GenerateMigrationId();

    // Create migration tracking info
    EntityMigrationInfo& info = _entityMigrations[guid];
    info.guid = guid;
    info.oldCellId = oldCellId;
    info.newCellId = newCellId;
    info.state = MigrationState::TRANSFERRING;
    info.migrationStartTime = 0;  // Will be set from game time
    info.bufferedMessages.clear();
    info.snapshot = CreateMigrationSnapshot(obj);

    // Create migration request payload
    auto payload = std::make_shared<MigrationRequestPayload>();
    payload->snapshot = info.snapshot;
    payload->migrationId = migrationId;

    // Send migration request to new cell
    ActorMessage msg;
    msg.type = MessageType::MIGRATION_REQUEST;
    msg.sourceGuid = guid;
    msg.sourceCellId = oldCellId;
    msg.targetCellId = newCellId;
    msg.complexPayload = payload;

    SendMessage(newCellId, std::move(msg));

    LOG_DEBUG("server", "GhostActor: Initiated migration for entity {} from cell {} to cell {} (migrationId: {})",
        guid, oldCellId, newCellId, migrationId);
}

void CellActorManager::ProcessMigrationRequest(const ActorMessage& msg)
{
    if (msg.type != MessageType::MIGRATION_REQUEST)
        return;

    auto payload = std::static_pointer_cast<MigrationRequestPayload>(msg.complexPayload);
    if (!payload)
        return;

    uint64_t guid = msg.sourceGuid;
    uint32_t oldCellId = msg.sourceCellId;
    uint32_t newCellId = msg.targetCellId;

    // Grid ID is the array index directly — cell is already pre-allocated
    CellActor* newCell = _cellActors[newCellId];

    // Accept the migration - entity will be added here
    // In a full implementation, we'd reconstruct the entity from the snapshot
    // For now, we just acknowledge

    // Send ACK back to old cell
    auto ackPayload = std::make_shared<MigrationAckPayload>();
    ackPayload->migrationId = payload->migrationId;
    ackPayload->accepted = true;

    ActorMessage ackMsg;
    ackMsg.type = MessageType::MIGRATION_ACK;
    ackMsg.sourceGuid = guid;
    ackMsg.sourceCellId = newCellId;
    ackMsg.targetCellId = oldCellId;
    ackMsg.complexPayload = ackPayload;

    SendMessage(oldCellId, std::move(ackMsg));

    LOG_DEBUG("server", "GhostActor: Accepted migration for entity {} into cell {} (migrationId: {})",
        guid, newCellId, payload->migrationId);
}

void CellActorManager::ProcessMigrationAck(const ActorMessage& msg)
{
    if (msg.type != MessageType::MIGRATION_ACK)
        return;

    auto payload = std::static_pointer_cast<MigrationAckPayload>(msg.complexPayload);
    if (!payload)
        return;

    uint64_t guid = msg.sourceGuid;

    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return;

    EntityMigrationInfo& info = it->second;

    if (!payload->accepted)
    {
        // Migration rejected - abort
        AbortMigration(guid);
        return;
    }

    // Migration accepted - forward buffered messages
    info.state = MigrationState::COMPLETING;
    ForwardBufferedMessages(info);

    // Send completion message
    ActorMessage completeMsg;
    completeMsg.type = MessageType::MIGRATION_COMPLETE;
    completeMsg.sourceGuid = guid;
    completeMsg.sourceCellId = info.oldCellId;
    completeMsg.targetCellId = info.newCellId;

    SendMessage(info.newCellId, std::move(completeMsg));

    // Clean up ghosts - destroy in old neighbors, create in new neighbors
    CleanupGhostsForMigration(guid, info.oldCellId, info.newCellId);

    // Complete migration
    CompleteMigration(guid);

    LOG_DEBUG("server", "GhostActor: Completed migration for entity {} to cell {}",
        guid, info.newCellId);
}

void CellActorManager::ProcessMigrationComplete(const ActorMessage& msg)
{
    if (msg.type != MessageType::MIGRATION_COMPLETE)
        return;

    // New cell acknowledges that migration is complete
    // Entity is now fully owned by new cell
    LOG_DEBUG("server", "GhostActor: Migration complete acknowledged for entity {} in cell {}",
        msg.sourceGuid, msg.targetCellId);
}

void CellActorManager::ForwardBufferedMessages(EntityMigrationInfo& info)
{
    for (ActorMessage& msg : info.bufferedMessages)
    {
        // Wrap in MIGRATION_FORWARD type
        ActorMessage forwardMsg;
        forwardMsg.type = MessageType::MIGRATION_FORWARD;
        forwardMsg.sourceGuid = info.guid;
        forwardMsg.targetCellId = info.newCellId;

        // Store original message in payload
        auto originalMsg = std::make_shared<ActorMessage>(std::move(msg));
        forwardMsg.complexPayload = originalMsg;

        SendMessage(info.newCellId, std::move(forwardMsg));
    }

    info.bufferedMessages.clear();
}

void CellActorManager::CompleteMigration(uint64_t guid)
{
    _entityMigrations.erase(guid);
}

void CellActorManager::AbortMigration(uint64_t guid)
{
    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return;

    LOG_WARN("server", "GhostActor: Aborting migration for entity {}", guid);

    // Clear buffered messages - they'll be processed in old cell
    it->second.bufferedMessages.clear();
    it->second.state = MigrationState::IDLE;

    _entityMigrations.erase(it);
}

void CellActorManager::CleanupGhostsForMigration(uint64_t guid, uint32_t oldCellId, uint32_t newCellId)
{
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    EntityGhostInfo& ghostInfo = it->second;

    // Update home cell to new cell
    ghostInfo.homeCellId = newCellId;

    // Ghost management will be updated on next UpdateEntityGhosts call
    // For now, just mark that ghosts need recalculation
    ghostInfo.activeGhosts = NeighborFlags::NONE;
}

bool CellActorManager::IsEntityMigrating(uint64_t guid) const
{
    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return false;

    return it->second.state != MigrationState::IDLE;
}

void CellActorManager::BufferMessageForMigrating(uint64_t guid, const ActorMessage& msg)
{
    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return;

    EntityMigrationInfo& info = it->second;
    if (info.state == MigrationState::TRANSFERRING)
    {
        info.bufferedMessages.push_back(msg);
    }
}

void CellActorManager::UpdateMigrations(uint32_t /*diff*/)
{
    // Check for migration timeouts
    std::vector<uint64_t> timedOut;

    for (auto& [guid, info] : _entityMigrations)
    {
        if (info.state == MigrationState::TRANSFERRING)
        {
            // TODO: Check actual time elapsed
            // For now, migrations complete quickly via message passing
            // If we wanted timeout support, we'd track start time and check diff
        }
    }

    for (uint64_t guid : timedOut)
    {
        AbortMigration(guid);
    }
}

// ============================================================================
// Cross-Cell Combat Helpers
// ============================================================================

uint32_t CellActorManager::GetCellIdForPosition(float x, float y) const
{
    return CalculateGridId(x, y);
}

uint32_t CellActorManager::GetCellIdForEntity(WorldObject* obj) const
{
    if (!obj)
        return 0;

    return GetCellIdForPosition(obj->GetPositionX(), obj->GetPositionY());
}

bool CellActorManager::AreInSameCell(WorldObject* a, WorldObject* b) const
{
    if (!a || !b)
        return false;

    return GetCellIdForEntity(a) == GetCellIdForEntity(b);
}

bool CellActorManager::AreInSameCell(float x1, float y1, float x2, float y2) const
{
    return GetCellIdForPosition(x1, y1) == GetCellIdForPosition(x2, y2);
}

// ============================================================================
// Threat/AI Integration
// ============================================================================

std::vector<uint32_t> CellActorManager::GetCellsInRadius(float x, float y, float radius) const
{
    std::vector<uint32_t> cells;

    // Calculate how many grids the radius spans
    // +1 to be conservative and catch edge cases
    int32_t gridRadius = static_cast<int32_t>(std::ceil(radius / GRID_SIZE)) + 1;

    // Get center grid coordinates
    uint32_t centerGridId = CalculateGridId(x, y);
    uint32_t centerGridX, centerGridY;
    ExtractGridCoords(centerGridId, centerGridX, centerGridY);
    int32_t cx = static_cast<int32_t>(centerGridX);
    int32_t cy = static_cast<int32_t>(centerGridY);

    constexpr float CENTER = GRIDS_PER_DIMENSION / 2.0f;

    // Iterate through potential grids in a square around center
    for (int32_t dy = -gridRadius; dy <= gridRadius; ++dy)
    {
        for (int32_t dx = -gridRadius; dx <= gridRadius; ++dx)
        {
            int32_t gx = cx + dx;
            int32_t gy = cy + dy;

            // Skip invalid grid coordinates
            if (gx < 0 || gy < 0 || gx >= static_cast<int32_t>(GRIDS_PER_DIMENSION) ||
                gy >= static_cast<int32_t>(GRIDS_PER_DIMENSION))
                continue;

            // Calculate center of this grid in world coords
            float gridCenterX = (CENTER - gx - 0.5f) * GRID_SIZE;
            float gridCenterY = (CENTER - gy - 0.5f) * GRID_SIZE;

            // Check if any part of the grid is within radius
            // Use distance to closest point in grid (clamped)
            float closestX = std::max(gridCenterX - GRID_SIZE / 2.0f,
                             std::min(x, gridCenterX + GRID_SIZE / 2.0f));
            float closestY = std::max(gridCenterY - GRID_SIZE / 2.0f,
                             std::min(y, gridCenterY + GRID_SIZE / 2.0f));

            float distSq = (closestX - x) * (closestX - x) + (closestY - y) * (closestY - y);

            if (distSq <= radius * radius)
            {
                cells.push_back(static_cast<uint32_t>(gy) * GRIDS_PER_DIMENSION +
                                static_cast<uint32_t>(gx));
            }
        }
    }

    return cells;
}

void CellActorManager::DoZoneInCombatCellAware(WorldObject* creature, float maxRange)
{
    if (!creature)
        return;

    BroadcastAggroRequest(creature, maxRange, 0.0f);
}

void CellActorManager::BroadcastAggroRequest(WorldObject* creature, float maxRange, float initialThreat)
{
    if (!creature)
        return;

    float x = creature->GetPositionX();
    float y = creature->GetPositionY();
    float z = creature->GetPositionZ();

    uint32_t creatureCellId = GetCellIdForEntity(creature);
    uint64_t creatureGuid = creature->GetGUID().GetRawValue();

    // Get all cells within the aggro range
    std::vector<uint32_t> nearbyCells = GetCellsInRadius(x, y, maxRange);

    LOG_DEBUG("server.ghost", "CellActorManager::BroadcastAggroRequest: creature={} at ({:.1f},{:.1f}) range={:.1f} cells={}",
        creatureGuid, x, y, maxRange, nearbyCells.size());

    // Send AGGRO_REQUEST to each nearby cell
    for (uint32_t cellId : nearbyCells)
    {
        auto payload = std::make_shared<AggroRequestPayload>();
        payload->creatureGuid = creatureGuid;
        payload->creatureCellId = creatureCellId;
        payload->creatureX = x;
        payload->creatureY = y;
        payload->creatureZ = z;
        payload->maxRange = maxRange;
        payload->initialThreat = initialThreat;
        payload->creaturePhaseMask = creature->GetPhaseMask();

        ActorMessage msg;
        msg.type = MessageType::AGGRO_REQUEST;
        msg.sourceGuid = creatureGuid;
        msg.sourceCellId = creatureCellId;
        msg.targetCellId = cellId;
        msg.complexPayload = payload;

        SendMessage(cellId, std::move(msg));
    }
}

// ============================================================================
// Parallel AI Integration
// ============================================================================

void CellActorManager::BroadcastAssistanceRequest(WorldObject* caller, uint64_t targetGuid, float radius)
{
    if (!caller)
        return;

    float x = caller->GetPositionX();
    float y = caller->GetPositionY();
    float z = caller->GetPositionZ();

    uint32_t callerCellId = GetCellIdForEntity(caller);
    uint64_t callerGuid = caller->GetGUID().GetRawValue();

    // Get all cells within the assistance radius
    std::vector<uint32_t> nearbyCells = GetCellsInRadius(x, y, radius);

    LOG_DEBUG("server.ghost", "CellActorManager::BroadcastAssistanceRequest: caller={} target={} at ({:.1f},{:.1f}) radius={:.1f} cells={}",
        callerGuid, targetGuid, x, y, radius, nearbyCells.size());

    // Send ASSISTANCE_REQUEST to each nearby cell
    for (uint32_t cellId : nearbyCells)
    {
        auto payload = std::make_shared<AssistanceRequestPayload>();
        payload->callerGuid = callerGuid;
        payload->targetGuid = targetGuid;
        payload->callerCellId = callerCellId;
        payload->callerX = x;
        payload->callerY = y;
        payload->callerZ = z;
        payload->radius = radius;
        payload->callerPhaseMask = caller->GetPhaseMask();

        ActorMessage msg;
        msg.type = MessageType::ASSISTANCE_REQUEST;
        msg.sourceGuid = callerGuid;
        msg.targetGuid = targetGuid;
        msg.sourceCellId = callerCellId;
        msg.targetCellId = cellId;
        msg.complexPayload = payload;

        SendMessage(cellId, std::move(msg));
    }
}

// ============================================================================
// Pet Parallelization Safety
// ============================================================================

void CellActorManager::QueuePetRemoval(WorldObject* pet, uint8_t saveMode, bool returnReagent)
{
    if (!pet)
        return;

    // Get the pet as a Pet* to access owner (ToPet is on Unit, not WorldObject)
    Unit* unitPet = pet->ToUnit();
    if (!unitPet)
        return;

    Pet* petObj = unitPet->ToPet();
    if (!petObj)
        return;

    Player* owner = petObj->GetOwner();
    if (!owner)
        return;

    uint32_t ownerCellId = GetCellIdForEntity(owner);

    auto payload = std::make_shared<PetRemovalPayload>();
    payload->petGuid = pet->GetGUID().GetRawValue();
    payload->ownerGuid = owner->GetGUID().GetRawValue();
    payload->saveMode = saveMode;
    payload->returnReagent = returnReagent;

    ActorMessage msg;
    msg.type = MessageType::PET_REMOVAL;
    msg.sourceGuid = pet->GetGUID().GetRawValue();
    msg.targetGuid = owner->GetGUID().GetRawValue();
    msg.sourceCellId = GetCellIdForEntity(pet);
    msg.targetCellId = ownerCellId;
    msg.complexPayload = payload;

    SendMessage(ownerCellId, std::move(msg));

    LOG_DEBUG("server.ghost", "CellActorManager::QueuePetRemoval: pet={} owner={} mode={} queued to cell {}",
        pet->GetGUID().GetRawValue(), owner->GetGUID().GetRawValue(), saveMode, ownerCellId);
}

// ============================================================================
// Cell-Aware Threat Management
// ============================================================================

void CellActorManager::AddThreatCellAware(WorldObject* attacker, WorldObject* victim, float threat)
{
    if (!attacker || !victim)
        return;

    if (!CanInteractCrossPhase(attacker, victim->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    if (attackerCellId == victimCellId)
    {
        // Same cell - direct threat modification is safe
        // The actual ThreatMgr::AddThreat call happens at the caller level
        LOG_DEBUG("server.ghost", "AddThreatCellAware: same cell, direct threat ok attacker={} victim={} threat={:.1f}",
            attacker->GetGUID().GetRawValue(), victim->GetGUID().GetRawValue(), threat);
    }
    else
    {
        // Different cells - send threat update message
        // The victim's cell will process this and update the bidirectional links
        SendThreatUpdate(
            attacker->GetGUID().GetRawValue(),
            victim->GetGUID().GetRawValue(),
            victimCellId,
            threat,
            true,   // isNewThreat - caller should check, but default to true
            false); // isRemoval
    }
}

void CellActorManager::RemoveThreatCellAware(WorldObject* attacker, WorldObject* victim)
{
    if (!attacker || !victim)
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    if (attackerCellId == victimCellId)
    {
        // Same cell - direct removal is safe
        LOG_DEBUG("server.ghost", "RemoveThreatCellAware: same cell, direct removal ok attacker={} victim={}",
            attacker->GetGUID().GetRawValue(), victim->GetGUID().GetRawValue());
    }
    else
    {
        // Different cells - send threat removal message
        SendThreatUpdate(
            attacker->GetGUID().GetRawValue(),
            victim->GetGUID().GetRawValue(),
            victimCellId,
            0.0f,
            false,  // isNewThreat
            true);  // isRemoval
    }
}

void CellActorManager::ModifyThreatByPercentCellAware(WorldObject* attacker, WorldObject* victim, int32_t percent)
{
    if (!attacker || !victim)
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    if (attackerCellId == victimCellId)
    {
        // Same cell - direct modification is safe
        LOG_DEBUG("server.ghost", "ModifyThreatByPercentCellAware: same cell, direct modify ok attacker={} victim={} pct={}",
            attacker->GetGUID().GetRawValue(), victim->GetGUID().GetRawValue(), percent);
    }
    else
    {
        // Different cells - send threat percentage modification message
        SendThreatUpdate(
            attacker->GetGUID().GetRawValue(),
            victim->GetGUID().GetRawValue(),
            attackerCellId,  // Message goes to creature's cell (threat list is on creature)
            0.0f,
            false,           // isNewThreat
            false,           // isRemoval
            percent);        // percentModify
    }
}

void CellActorManager::SendThreatUpdate(uint64_t attackerGuid, uint64_t victimGuid, uint32_t victimCellId,
                                         float threatDelta, bool isNewThreat, bool isRemoval, int32_t percentModify)
{
    auto payload = std::make_shared<ThreatUpdatePayload>();
    payload->attackerGuid = attackerGuid;
    payload->victimGuid = victimGuid;
    payload->threatDelta = threatDelta;
    payload->percentModify = percentModify;
    payload->isNewThreat = isNewThreat;
    payload->isRemoval = isRemoval;

    ActorMessage msg;
    msg.type = MessageType::THREAT_UPDATE;
    msg.sourceGuid = attackerGuid;
    msg.targetGuid = victimGuid;
    msg.targetCellId = victimCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendThreatUpdate: attacker={} victim={} cell={} delta={:.1f} new={} remove={} pct={}",
        attackerGuid, victimGuid, victimCellId, threatDelta, isNewThreat, isRemoval, percentModify);

    SendMessage(victimCellId, std::move(msg));
}

// ============================================================================
// Cross-Cell Damage/Healing Message Senders
// ============================================================================

void CellActorManager::SendSpellHitMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t damage, int32_t healing)
{
    if (!target)
        return;

    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<SpellHitPayload>();
    payload->spellId = spellId;
    payload->damage = damage;
    payload->healing = healing;
    payload->effectMask = 1;

    ActorMessage msg{};
    msg.type = MessageType::SPELL_HIT;
    msg.sourceGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = caster ? GetCellIdForEntity(caster) : 0;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendSpellHitMessage: spell={} caster={} target={} damage={} healing={}",
        spellId, msg.sourceGuid, msg.targetGuid, damage, healing);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendMeleeDamageMessage(Unit* attacker, Unit* target, int32_t damage, bool isCrit)
{
    if (!target)
        return;

    if (attacker && !CanInteractCrossPhase(attacker, target->GetGUID().GetRawValue()))
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<MeleeDamagePayload>();
    payload->damage = damage;
    payload->isCritical = isCrit;

    ActorMessage msg{};
    msg.type = MessageType::MELEE_DAMAGE;
    msg.sourceGuid = attacker ? attacker->GetGUID().GetRawValue() : 0;
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = attacker ? GetCellIdForEntity(attacker) : 0;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendMeleeDamageMessage: attacker={} target={} damage={} crit={}",
        msg.sourceGuid, msg.targetGuid, damage, isCrit);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendHealMessage(Unit* healer, Unit* target, uint32_t spellId, int32_t amount)
{
    if (!target)
        return;

    if (healer && !CanInteractCrossPhase(healer, target->GetGUID().GetRawValue()))
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<HealPayload>();
    payload->spellId = spellId;
    payload->healAmount = amount;
    payload->effectiveHeal = amount;

    ActorMessage msg{};
    msg.type = MessageType::HEAL;
    msg.sourceGuid = healer ? healer->GetGUID().GetRawValue() : 0;
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = healer ? GetCellIdForEntity(healer) : 0;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendHealMessage: spell={} healer={} target={} amount={}",
        spellId, msg.sourceGuid, msg.targetGuid, amount);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendReflectDamageMessage(Unit* reflector, Unit* attacker, uint32_t spellId,
    int32_t damage, uint32_t schoolMask, uint32_t absorb, uint32_t resist)
{
    if (!attacker || !reflector)
        return;

    // Phase check - reflector must be visible to attacker
    if (!CanInteractCrossPhase(reflector, attacker->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);

    auto payload = std::make_shared<ReflectDamagePayload>();
    payload->reflectorGuid = reflector->GetGUID().GetRawValue();
    payload->attackerGuid = attacker->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->damage = damage;
    payload->schoolMask = schoolMask;
    payload->absorb = absorb;
    payload->resist = resist;

    ActorMessage msg{};
    msg.type = MessageType::REFLECT_DAMAGE;
    msg.sourceGuid = reflector->GetGUID().GetRawValue();
    msg.targetGuid = attacker->GetGUID().GetRawValue();
    msg.sourceCellId = GetCellIdForEntity(reflector);
    msg.targetCellId = attackerCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendReflectDamageMessage: reflector={} attacker={} spell={} damage={}",
        msg.sourceGuid, msg.targetGuid, spellId, damage);

    SendMessage(attackerCellId, std::move(msg));
}

void CellActorManager::SendTargetSwitchMessage(Unit* creature, uint64_t oldTargetGuid, uint64_t newTargetGuid)
{
    if (!creature)
        return;

    auto payload = std::make_shared<TargetSwitchPayload>();
    payload->creatureGuid = creature->GetGUID().GetRawValue();
    payload->oldTargetGuid = oldTargetGuid;
    payload->newTargetGuid = newTargetGuid;

    ActorMessage msg{};
    msg.type = MessageType::TARGET_SWITCH;
    msg.sourceGuid = creature->GetGUID().GetRawValue();
    msg.complexPayload = payload;

    // Broadcast to all neighboring cells so they can update their ghosts
    uint32_t creatureCellId = GetCellIdForEntity(creature);
    std::vector<uint32_t> neighbors = GetNeighborCellIds(creatureCellId);

    LOG_DEBUG("server.ghost", "SendTargetSwitchMessage: creature={} cell={} old={} new={} neighbors={}",
        msg.sourceGuid, creatureCellId, oldTargetGuid, newTargetGuid, neighbors.size());

    for (uint32_t neighborCellId : neighbors)
    {
        SendMessage(neighborCellId, msg);
    }
}

void CellActorManager::BroadcastEvadeTriggered(Unit* creature)
{
    if (!creature)
        return;

    ActorMessage msg{};
    msg.type = MessageType::EVADE_TRIGGERED;
    msg.sourceGuid = creature->GetGUID().GetRawValue();

    uint32_t creatureCellId = GetCellIdForEntity(creature);
    std::vector<uint32_t> neighbors = GetNeighborCellIds(creatureCellId);

    LOG_DEBUG("server.ghost", "BroadcastEvadeTriggered: creature={} cell={}",
        msg.sourceGuid, creatureCellId);

    for (uint32_t neighborCellId : neighbors)
    {
        SendMessage(neighborCellId, msg);
    }
}

void CellActorManager::SendInterruptMessage(Unit* caster, Unit* target, uint32_t interruptSpellId,
                                             uint32_t interruptedSpellId, uint32_t schoolMask, int32_t lockoutDuration)
{
    if (!target)
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<SpellInterruptPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->interruptSpellId = interruptSpellId;
    payload->interruptedSpellId = interruptedSpellId;
    payload->schoolMask = schoolMask;
    payload->lockoutDuration = lockoutDuration;

    ActorMessage msg{};
    msg.type = MessageType::SPELL_INTERRUPT;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendInterruptMessage: caster={} target={} spell={} interrupted={} school={} lockout={}",
        payload->casterGuid, payload->targetGuid, interruptSpellId, interruptedSpellId, schoolMask, lockoutDuration);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendDispelMessage(Unit* caster, Unit* target, uint32_t dispelSpellId,
                                          const std::vector<std::pair<uint32_t, uint8_t>>& dispelList)
{
    if (!target || dispelList.empty())
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<SpellDispelPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->dispelSpellId = dispelSpellId;
    payload->dispelList = dispelList;

    ActorMessage msg{};
    msg.type = MessageType::SPELL_DISPEL;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendDispelMessage: caster={} target={} spell={} count={}",
        payload->casterGuid, payload->targetGuid, dispelSpellId, dispelList.size());

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendPowerDrainMessage(Unit* caster, Unit* target, uint32_t spellId, uint8_t powerType,
                                              int32_t amount, float gainMultiplier, bool isPowerBurn)
{
    if (!target || amount <= 0)
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<PowerDrainPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->powerType = powerType;
    payload->amount = amount;
    payload->gainMultiplier = gainMultiplier;
    payload->isPowerBurn = isPowerBurn;

    ActorMessage msg{};
    msg.type = MessageType::POWER_DRAIN;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendPowerDrainMessage: caster={} target={} spell={} powerType={} amount={} isPowerBurn={}",
        payload->casterGuid, payload->targetGuid, spellId, powerType, amount, isPowerBurn);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendSpellstealMessage(Unit* caster, Unit* target, uint32_t spellstealSpellId,
                                              const std::vector<std::pair<uint32_t, uint64_t>>& stealList)
{
    if (!target || stealList.empty())
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<SpellstealPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellstealSpellId = spellstealSpellId;
    payload->stealList = stealList;

    ActorMessage msg{};
    msg.type = MessageType::SPELLSTEAL;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendSpellstealMessage: caster={} target={} spell={} count={}",
        payload->casterGuid, payload->targetGuid, spellstealSpellId, stealList.size());

    SendMessage(targetCellId, std::move(msg));
}

// ============================================================================
// Cell-Aware Victim Selection
// ============================================================================

CellActorManager::VictimInfo CellActorManager::GetVictimCellAware(WorldObject* attacker)
{
    VictimInfo result;
    if (!attacker)
        return result;

    Creature* creature = attacker->ToCreature();
    if (!creature)
        return result;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);

    // First try local victim (same cell) via direct pointer
    Unit* victim = creature->GetVictim();
    if (victim)
    {
        result.guid = victim->GetGUID().GetRawValue();
        result.isLocal = AreInSameCell(attacker, victim);
        result.posX = victim->GetPositionX();
        result.posY = victim->GetPositionY();
        result.posZ = victim->GetPositionZ();

        LOG_DEBUG("server.ghost", "GetVictimCellAware: attacker={} cell={} victim={} local={}",
            attacker->GetGUID().GetRawValue(), attackerCellId, result.guid, result.isLocal);
        return result;
    }

    // No direct victim - check ThreatMgr for current victim
    ThreatMgr& threatMgr = creature->GetThreatMgr();
    if (Unit* threatVictim = threatMgr.GetCurrentVictim())
    {
        result.guid = threatVictim->GetGUID().GetRawValue();
        result.isLocal = AreInSameCell(attacker, threatVictim);
        result.posX = threatVictim->GetPositionX();
        result.posY = threatVictim->GetPositionY();
        result.posZ = threatVictim->GetPositionZ();

        LOG_DEBUG("server.ghost", "GetVictimCellAware: attacker={} cell={} threatVictim={} local={}",
            attacker->GetGUID().GetRawValue(), attackerCellId, result.guid, result.isLocal);
    }

    return result;
}

// ============================================================================
// Cell-Managed Object Tracking
// ============================================================================

bool CellActorManager::IsCellManaged(WorldObject* obj) const
{
    if (!obj)
        return false;

    // O(1) flag check instead of hash lookup
    return obj->IsCellManaged();
}

bool CellActorManager::IsCellManagedByGuid([[maybe_unused]] uint64_t guid) const
{
    // Cannot check by GUID alone - need the object pointer for the flag
    // This method is deprecated in favor of IsCellManaged(WorldObject*)
    return false;
}

// =============================================================================
// Cross-cell Control Effects
// =============================================================================

void CellActorManager::SendStunMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration)
{
    if (!target)
        return;

    // Phase check
    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = caster ? GetCellIdForEntity(caster) : 0;
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, stun happens directly through normal code paths
    if (caster && casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendStunMessage: same cell, handled directly caster={} target={}",
            caster->GetGUID().GetRawValue(), target->GetGUID().GetRawValue());
        return;
    }

    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = UNIT_STATE_STUNNED;

    ActorMessage msg{};
    msg.type = MessageType::STUN;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.sourceCellId = casterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendStunMessage: caster={} target={} spell={} duration={}ms",
        msg.sourceGuid, msg.targetGuid, spellId, duration);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendRootMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration)
{
    if (!target)
        return;

    // Phase check
    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = caster ? GetCellIdForEntity(caster) : 0;
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, root happens directly through normal code paths
    if (caster && casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendRootMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = UNIT_STATE_ROOT;

    ActorMessage msg{};
    msg.type = MessageType::ROOT;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.sourceCellId = casterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendRootMessage: caster={} target={} spell={} duration={}ms",
        msg.sourceGuid, msg.targetGuid, spellId, duration);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendFearMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration,
                                       float destX, float destY, float destZ)
{
    if (!target)
        return;

    // Phase check
    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = caster ? GetCellIdForEntity(caster) : 0;
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, fear happens directly through normal code paths
    if (caster && casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendFearMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = UNIT_STATE_FLEEING;
    payload->fearDestX = destX;
    payload->fearDestY = destY;
    payload->fearDestZ = destZ;

    ActorMessage msg{};
    msg.type = MessageType::FEAR;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.sourceCellId = casterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendFearMessage: caster={} target={} spell={} dest=({:.1f}, {:.1f}, {:.1f})",
        msg.sourceGuid, msg.targetGuid, spellId, destX, destY, destZ);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendCharmMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration)
{
    if (!caster || !target)
        return;

    // Phase check
    if (!CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = GetCellIdForEntity(caster);
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, charm notification is handled directly
    if (casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendCharmMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster->GetGUID().GetRawValue();
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = 0;  // Charm doesn't use a simple UnitState

    ActorMessage msg{};
    msg.type = MessageType::CHARM;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.sourceCellId = casterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendCharmMessage: caster={} target={} spell={} duration={}ms",
        msg.sourceGuid, msg.targetGuid, spellId, duration);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendKnockbackMessage(Unit* caster, Unit* target, uint32_t spellId,
                                            float speedXY, float speedZ,
                                            float destX, float destY, float destZ)
{
    if (!target)
        return;

    // Phase check
    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = caster ? GetCellIdForEntity(caster) : 0;
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, knockback happens directly through normal code paths
    if (caster && casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendKnockbackMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<KnockbackPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->originX = caster ? caster->GetPositionX() : target->GetPositionX();
    payload->originY = caster ? caster->GetPositionY() : target->GetPositionY();
    payload->originZ = caster ? caster->GetPositionZ() : target->GetPositionZ();
    payload->speedXY = speedXY;
    payload->speedZ = speedZ;
    payload->destX = destX;
    payload->destY = destY;
    payload->destZ = destZ;

    ActorMessage msg{};
    msg.type = MessageType::KNOCKBACK;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.sourceCellId = casterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendKnockbackMessage: caster={} target={} velocity=({:.1f}, {:.1f})",
        msg.sourceGuid, msg.targetGuid, speedXY, speedZ);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendSilenceMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration)
{
    if (!target)
        return;

    // Phase check
    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = caster ? GetCellIdForEntity(caster) : 0;
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, silence happens directly through normal code paths
    if (caster && casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendSilenceMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = 0;  // Silence uses flag, not UnitState

    ActorMessage msg{};
    msg.type = MessageType::SILENCE;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.sourceCellId = casterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendSilenceMessage: caster={} target={} spell={} duration={}ms",
        msg.sourceGuid, msg.targetGuid, spellId, duration);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendPolymorphMessage(Unit* caster, Unit* target, uint32_t spellId,
                                            int32_t duration, uint32_t transformDisplayId)
{
    if (!target)
        return;

    // Phase check
    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = caster ? GetCellIdForEntity(caster) : 0;
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, polymorph happens directly through normal code paths
    if (caster && casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendPolymorphMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = UNIT_STATE_STUNNED;  // Polymorph is an incapacitate
    payload->transformDisplayId = transformDisplayId;

    ActorMessage msg{};
    msg.type = MessageType::POLYMORPH;
    msg.sourceGuid = payload->casterGuid;
    msg.targetGuid = payload->targetGuid;
    msg.sourceCellId = casterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendPolymorphMessage: caster={} target={} spell={} displayId={}",
        msg.sourceGuid, msg.targetGuid, spellId, transformDisplayId);

    SendMessage(targetCellId, std::move(msg));
}

// =============================================================================
// Combat Feedback
// =============================================================================

void CellActorManager::SendDodgeMessage(Unit* attacker, Unit* victim, uint32_t spellId)
{
    if (!attacker || !victim)
        return;

    // Phase check
    if (!CanInteractCrossPhase(attacker, victim->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    // Same cell - handled directly through normal code paths
    if (attackerCellId == victimCellId)
    {
        LOG_DEBUG("ghostactor", "SendDodgeMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<CombatResultPayload>();
    payload->attackerGuid = attacker->GetGUID().GetRawValue();
    payload->victimGuid = victim->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->resultType = MELEE_HIT_DODGE;
    payload->procEx = PROC_EX_DODGE;

    ActorMessage msg{};
    msg.type = MessageType::DODGE;
    msg.sourceGuid = payload->victimGuid;  // Victim dodged
    msg.targetGuid = payload->attackerGuid;  // Attacker needs to know
    msg.sourceCellId = victimCellId;
    msg.targetCellId = attackerCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendDodgeMessage: attacker={} victim={} spell={}",
        payload->attackerGuid, payload->victimGuid, spellId);

    // Send to attacker's cell so they can trigger procs like Overpower
    SendMessage(attackerCellId, std::move(msg));
}

void CellActorManager::SendParryMessage(Unit* attacker, Unit* victim, uint32_t spellId)
{
    if (!attacker || !victim)
        return;

    // Phase check
    if (!CanInteractCrossPhase(attacker, victim->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    // Same cell - handled directly
    if (attackerCellId == victimCellId)
    {
        LOG_DEBUG("ghostactor", "SendParryMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<CombatResultPayload>();
    payload->attackerGuid = attacker->GetGUID().GetRawValue();
    payload->victimGuid = victim->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->resultType = MELEE_HIT_PARRY;
    payload->procEx = PROC_EX_PARRY;

    ActorMessage msg{};
    msg.type = MessageType::PARRY;
    msg.sourceGuid = payload->victimGuid;
    msg.targetGuid = payload->attackerGuid;
    msg.sourceCellId = victimCellId;
    msg.targetCellId = attackerCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendParryMessage: attacker={} victim={} spell={}",
        payload->attackerGuid, payload->victimGuid, spellId);

    // Send to attacker's cell for notification
    SendMessage(attackerCellId, std::move(msg));
}

void CellActorManager::SendBlockMessage(Unit* attacker, Unit* victim, uint32_t spellId, int32_t blockedAmount)
{
    if (!attacker || !victim)
        return;

    // Phase check
    if (!CanInteractCrossPhase(attacker, victim->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    // Same cell - handled directly
    if (attackerCellId == victimCellId)
    {
        LOG_DEBUG("ghostactor", "SendBlockMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<CombatResultPayload>();
    payload->attackerGuid = attacker->GetGUID().GetRawValue();
    payload->victimGuid = victim->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->resultType = MELEE_HIT_BLOCK;
    payload->blockedAmount = blockedAmount;
    payload->procEx = PROC_EX_BLOCK;

    ActorMessage msg{};
    msg.type = MessageType::BLOCK;
    msg.sourceGuid = payload->victimGuid;
    msg.targetGuid = payload->attackerGuid;
    msg.sourceCellId = victimCellId;
    msg.targetCellId = attackerCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendBlockMessage: attacker={} victim={} blocked={}",
        payload->attackerGuid, payload->victimGuid, blockedAmount);

    SendMessage(attackerCellId, std::move(msg));
}

void CellActorManager::SendMissMessage(Unit* attacker, Unit* victim, uint32_t spellId)
{
    if (!attacker || !victim)
        return;

    // Phase check
    if (!CanInteractCrossPhase(attacker, victim->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    // Same cell - handled directly
    if (attackerCellId == victimCellId)
    {
        LOG_DEBUG("ghostactor", "SendMissMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<CombatResultPayload>();
    payload->attackerGuid = attacker->GetGUID().GetRawValue();
    payload->victimGuid = victim->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->resultType = MELEE_HIT_MISS;
    payload->procEx = PROC_EX_MISS;

    ActorMessage msg{};
    msg.type = MessageType::MISS;
    msg.sourceGuid = payload->attackerGuid;
    msg.targetGuid = payload->victimGuid;
    msg.sourceCellId = attackerCellId;
    msg.targetCellId = victimCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendMissMessage: attacker={} victim={} spell={}",
        payload->attackerGuid, payload->victimGuid, spellId);

    // Attacker missed - notify attacker's cell
    SendMessage(attackerCellId, std::move(msg));
}

void CellActorManager::SendImmuneMessage(Unit* caster, Unit* target, uint32_t spellId)
{
    if (!caster || !target)
        return;

    // Phase check
    if (!CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t casterCellId = GetCellIdForEntity(caster);
    uint32_t targetCellId = GetCellIdForEntity(target);

    // Same cell - handled directly
    if (casterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendImmuneMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<CombatResultPayload>();
    payload->attackerGuid = caster->GetGUID().GetRawValue();
    payload->victimGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->procEx = PROC_EX_IMMUNE;

    ActorMessage msg{};
    msg.type = MessageType::IMMUNE;
    msg.sourceGuid = payload->victimGuid;
    msg.targetGuid = payload->attackerGuid;
    msg.sourceCellId = targetCellId;
    msg.targetCellId = casterCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendImmuneMessage: caster={} target={} spell={}",
        payload->attackerGuid, payload->victimGuid, spellId);

    // Notify caster's cell that target was immune
    SendMessage(casterCellId, std::move(msg));
}

void CellActorManager::SendAbsorbMessage(Unit* attacker, Unit* victim, uint32_t spellId, int32_t absorbedAmount)
{
    if (!attacker || !victim)
        return;

    // Phase check
    if (!CanInteractCrossPhase(attacker, victim->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    // Same cell - handled directly
    if (attackerCellId == victimCellId)
    {
        LOG_DEBUG("ghostactor", "SendAbsorbMessage: same cell, handled directly");
        return;
    }

    auto payload = std::make_shared<CombatResultPayload>();
    payload->attackerGuid = attacker->GetGUID().GetRawValue();
    payload->victimGuid = victim->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->absorbedAmount = absorbedAmount;
    payload->procEx = PROC_EX_ABSORB;

    ActorMessage msg{};
    msg.type = MessageType::ABSORB_NOTIFICATION;
    msg.sourceGuid = payload->victimGuid;
    msg.targetGuid = payload->attackerGuid;
    msg.sourceCellId = victimCellId;
    msg.targetCellId = attackerCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendAbsorbMessage: attacker={} victim={} absorbed={}",
        payload->attackerGuid, payload->victimGuid, absorbedAmount);

    // Notify attacker's cell about the absorbed damage
    SendMessage(attackerCellId, std::move(msg));
}

// =============================================================================
// Spell Cast Notifications - Broadcast to neighboring cells for AI reactions
// =============================================================================

void CellActorManager::SendSpellCastStartMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t castTime)
{
    if (!caster)
        return;

    uint32_t casterCellId = GetCellIdForEntity(caster);

    auto payload = std::make_shared<SpellCastPayload>();
    payload->casterGuid = caster->GetGUID().GetRawValue();
    payload->targetGuid = target ? target->GetGUID().GetRawValue() : 0;
    payload->spellId = spellId;
    payload->castTime = castTime;

    // Get spell school mask for AI interrupt priority
    if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId))
        payload->schoolMask = spellInfo->GetSchoolMask();

    LOG_DEBUG("ghostactor", "SendSpellCastStartMessage: caster={} target={} spell={} castTime={}ms school={}",
        payload->casterGuid, payload->targetGuid, spellId, castTime, payload->schoolMask);

    // Broadcast to all neighboring cells so nearby AI can react
    std::vector<uint32_t> neighborCells = GetNeighborCellIds(casterCellId);
    for (uint32_t neighborCellId : neighborCells)
    {
        ActorMessage msg{};
        msg.type = MessageType::SPELL_CAST_START;
        msg.sourceGuid = payload->casterGuid;
        msg.targetGuid = payload->targetGuid;
        msg.sourceCellId = casterCellId;
        msg.targetCellId = neighborCellId;
        msg.complexPayload = payload;

        SendMessage(neighborCellId, std::move(msg));
    }
}

void CellActorManager::SendSpellCastFailedMessage(Unit* caster, uint32_t spellId, uint8_t failReason)
{
    if (!caster)
        return;

    uint32_t casterCellId = GetCellIdForEntity(caster);

    auto payload = std::make_shared<SpellCastPayload>();
    payload->casterGuid = caster->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->failReason = failReason;

    LOG_DEBUG("ghostactor", "SendSpellCastFailedMessage: caster={} spell={} reason={}",
        payload->casterGuid, spellId, failReason);

    // Broadcast to neighbors so AI knows the cast was interrupted
    std::vector<uint32_t> neighborCells = GetNeighborCellIds(casterCellId);
    for (uint32_t neighborCellId : neighborCells)
    {
        ActorMessage msg{};
        msg.type = MessageType::SPELL_CAST_FAILED;
        msg.sourceGuid = payload->casterGuid;
        msg.sourceCellId = casterCellId;
        msg.targetCellId = neighborCellId;
        msg.complexPayload = payload;

        SendMessage(neighborCellId, std::move(msg));
    }
}

void CellActorManager::SendSpellCastSuccessMessage(Unit* caster, Unit* target, uint32_t spellId)
{
    if (!caster)
        return;

    uint32_t casterCellId = GetCellIdForEntity(caster);

    auto payload = std::make_shared<SpellCastPayload>();
    payload->casterGuid = caster->GetGUID().GetRawValue();
    payload->targetGuid = target ? target->GetGUID().GetRawValue() : 0;
    payload->spellId = spellId;

    LOG_DEBUG("ghostactor", "SendSpellCastSuccessMessage: caster={} target={} spell={}",
        payload->casterGuid, payload->targetGuid, spellId);

    // Broadcast to neighbors for reactive AI behaviors
    std::vector<uint32_t> neighborCells = GetNeighborCellIds(casterCellId);
    for (uint32_t neighborCellId : neighborCells)
    {
        ActorMessage msg{};
        msg.type = MessageType::SPELL_CAST_SUCCESS;
        msg.sourceGuid = payload->casterGuid;
        msg.targetGuid = payload->targetGuid;
        msg.sourceCellId = casterCellId;
        msg.targetCellId = neighborCellId;
        msg.complexPayload = payload;

        SendMessage(neighborCellId, std::move(msg));
    }
}

// =============================================================================
// Taunt - Cross-cell taunt/detaunt for tank gameplay
// =============================================================================

void CellActorManager::SendTauntMessage(Unit* taunter, Unit* target, uint32_t spellId, int32_t duration)
{
    if (!taunter || !target)
        return;

    // Taunt only makes sense against creatures with threat lists
    if (!target->IsCreature() || !target->CanHaveThreatList())
        return;

    // Phase check
    if (!CanInteractCrossPhase(taunter, target->GetGUID().GetRawValue()))
        return;

    uint32_t taunterCellId = GetCellIdForEntity(taunter);
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, taunt happens directly through normal code paths
    if (taunterCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendTauntMessage: same cell, handled directly taunter={} target={}",
            taunter->GetGUID().GetRawValue(), target->GetGUID().GetRawValue());
        return;
    }

    auto payload = std::make_shared<TauntPayload>();
    payload->taunterGuid = taunter->GetGUID().GetRawValue();
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->duration = duration;
    payload->isSingleTarget = true;
    payload->threatAmount = 0.0f;  // Threat is calculated on receive

    ActorMessage msg{};
    msg.type = MessageType::TAUNT;
    msg.sourceGuid = taunter->GetGUID().GetRawValue();
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = taunterCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendTauntMessage: taunter={} cell={} -> creature={} cell={} spell={}",
        taunter->GetGUID().GetRawValue(), taunterCellId,
        target->GetGUID().GetRawValue(), targetCellId, spellId);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendDetauntMessage(Unit* source, Unit* target, uint32_t spellId, float threatReductionPct)
{
    if (!source || !target)
        return;

    // Detaunt only makes sense against creatures with threat lists
    if (!target->IsCreature() || !target->CanHaveThreatList())
        return;

    // Phase check
    if (!CanInteractCrossPhase(source, target->GetGUID().GetRawValue()))
        return;

    uint32_t sourceCellId = GetCellIdForEntity(source);
    uint32_t targetCellId = GetCellIdForEntity(target);

    // If same cell, detaunt happens directly through normal code paths
    if (sourceCellId == targetCellId)
    {
        LOG_DEBUG("ghostactor", "SendDetauntMessage: same cell, handled directly source={} target={}",
            source->GetGUID().GetRawValue(), target->GetGUID().GetRawValue());
        return;
    }

    auto payload = std::make_shared<DetauntPayload>();
    payload->sourceGuid = source->GetGUID().GetRawValue();
    payload->targetGuid = target->GetGUID().GetRawValue();
    payload->spellId = spellId;
    payload->threatReductionPct = threatReductionPct;
    payload->removeTaunt = (threatReductionPct <= 0.0f);  // If no threat reduction, it's a taunt fadeout

    ActorMessage msg{};
    msg.type = MessageType::DETAUNT;
    msg.sourceGuid = source->GetGUID().GetRawValue();
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = sourceCellId;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("ghostactor", "SendDetauntMessage: source={} cell={} -> creature={} cell={} threatPct={:.1f}%",
        source->GetGUID().GetRawValue(), sourceCellId,
        target->GetGUID().GetRawValue(), targetCellId, threatReductionPct);

    SendMessage(targetCellId, std::move(msg));
}

// =============================================================================
// Resurrection - STUBS
// =============================================================================

void CellActorManager::SendResurrectRequestMessage(Unit* caster, Unit* target, uint32_t spellId,
                                                   uint32_t healthPct, uint32_t manaPct)
{
    // TODO: Implement cross-cell resurrection request
    // Dead player receives accept/decline dialog
    if (!caster || !target)
        return;
    LOG_DEBUG("ghostactor", "SendResurrectRequestMessage: NOT IMPLEMENTED");
    (void)spellId;
    (void)healthPct;
    (void)manaPct;
}

void CellActorManager::SendResurrectAcceptMessage(Unit* target, uint64_t casterGuid)
{
    // TODO: Route acceptance back to caster's cell
    if (!target)
        return;
    LOG_DEBUG("ghostactor", "SendResurrectAcceptMessage: NOT IMPLEMENTED");
    (void)casterGuid;
}

// =============================================================================
// Player Social - STUBS
// =============================================================================

void CellActorManager::SendDuelRequestMessage(Player* challenger, Player* challenged)
{
    // TODO: Route duel challenge to target's cell
    if (!challenger || !challenged)
        return;
    LOG_DEBUG("ghostactor", "SendDuelRequestMessage: NOT IMPLEMENTED");
}

void CellActorManager::SendDuelStateMessage(Player* player1, Player* player2, uint8_t state, uint8_t result)
{
    // TODO: Sync duel state between cells
    if (!player1 || !player2)
        return;
    LOG_DEBUG("ghostactor", "SendDuelStateMessage: NOT IMPLEMENTED");
    (void)state;
    (void)result;
}

void CellActorManager::SendTradeRequestMessage(Player* initiator, Player* target)
{
    // TODO: Route trade request to target's cell
    if (!initiator || !target)
        return;
    LOG_DEBUG("ghostactor", "SendTradeRequestMessage: NOT IMPLEMENTED");
}

} // namespace GhostActor
