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

#include "CreatureScript.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "naxxramas.h"

enum Says
{
    SAY_AGGRO                       = 0,
    SAY_SLAY                        = 1,
    SAY_TAUNT                       = 2,
    EMOTE_DEATH                     = 3,
    EMOTE_DANCE                     = 4,
    EMOTE_DANCE_END                 = 5,
    SAY_DANCE                       = 6
};

enum Spells
{
    SPELL_SPELL_DISRUPTION          = 29310,
    SPELL_DECREPIT_FEVER            = 29998,
    SPELL_PLAGUE_CLOUD              = 29350,
    SPELL_TELEPORT_SELF             = 30211
};

enum Events
{
    EVENT_DISRUPTION                = 1,
    EVENT_DECREPIT_FEVER            = 2,
    EVENT_ERUPT_SECTION             = 3,
    EVENT_DANCE                     = 4,
    EVENT_DANCE_END                 = 5,
    EVENT_SAFETY_DANCE              = 6,
    EVENT_PLAGUE_CLOUD              = 7
};

enum Phases
{
    PHASE_FIGHT                     = 1,
    PHASE_DANCE                     = 2
};

struct boss_heigan : public BossAI
{
    boss_heigan(Creature* creature) : BossAI(creature, BOSS_HEIGAN) { }

    void Reset() override
    {
        BossAI::Reset();
        me->SetReactState(REACT_AGGRESSIVE);
        _currentSection = 3;
        _moveRight = true;
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        Talk(SAY_SLAY);
        instance->StorePersistentData(PERSISTENT_DATA_IMMORTAL_FAIL, 1);
    }

    void JustDied(Unit*  killer) override
    {
        BossAI::JustDied(killer);
        Talk(EMOTE_DEATH);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        me->SetInCombatWithZone();
        Talk(SAY_AGGRO);

        _currentSection = 3;
        events.SetPhase(PHASE_FIGHT);
        events.ScheduleEvent(EVENT_DISRUPTION, randtime(12s, 15s), 0, PHASE_FIGHT);
        events.ScheduleEvent(EVENT_DECREPIT_FEVER, 17s, 0, PHASE_FIGHT);
        events.ScheduleEvent(EVENT_DANCE, 90s, 0, PHASE_FIGHT);
        events.ScheduleEvent(EVENT_ERUPT_SECTION, 15s);
        events.ScheduleEvent(EVENT_SAFETY_DANCE, 5s);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_DISRUPTION:
                    DoCastSelf(SPELL_SPELL_DISRUPTION);
                    events.Repeat(10s);
                    break;
                case EVENT_DECREPIT_FEVER:
                    DoCastSelf(SPELL_DECREPIT_FEVER);
                    events.Repeat(randtime(22s, 25s));
                    break;
                case EVENT_DANCE:
                    events.SetPhase(PHASE_DANCE);
                    Talk(SAY_DANCE);
                    Talk(EMOTE_DANCE);
                    _currentSection = 3;
                    me->SetReactState(REACT_PASSIVE);
                    me->AttackStop();
                    me->StopMoving();
                    me->CastSpell(me, SPELL_TELEPORT_SELF, false);
                    me->SetFacingTo(2.40f);
                    events.ScheduleEvent(EVENT_PLAGUE_CLOUD, 1s, 0, PHASE_DANCE);
                    events.ScheduleEvent(EVENT_DANCE_END, 45s, 0, PHASE_DANCE);
                    events.RescheduleEvent(EVENT_ERUPT_SECTION, 7s);
                    break;
                case EVENT_PLAGUE_CLOUD:
                    DoCastSelf(SPELL_PLAGUE_CLOUD);
                    break;
                case EVENT_DANCE_END:
                    events.SetPhase(PHASE_FIGHT);
                    Talk(EMOTE_DANCE_END);
                    _currentSection = 3;
                    me->CastStop();
                    me->SetReactState(REACT_AGGRESSIVE);
                    DoZoneInCombat();
                    events.ScheduleEvent(EVENT_DISRUPTION, randtime(12s, 15s), 0, PHASE_FIGHT);
                    events.ScheduleEvent(EVENT_DECREPIT_FEVER, 17s, 0, PHASE_FIGHT);
                    events.ScheduleEvent(EVENT_DANCE, 90s, 0, PHASE_FIGHT);
                    events.RescheduleEvent(EVENT_ERUPT_SECTION, 15s);
                    break;
                case EVENT_ERUPT_SECTION:
                    instance->SetData(DATA_HEIGAN_ERUPTION, _currentSection);
                    if (_currentSection == 3)
                        _moveRight = false;
                    else if (_currentSection == 0)
                        _moveRight = true;

                    _moveRight ? _currentSection++ : _currentSection--;
                    Talk(SAY_TAUNT);
                    events.Repeat(events.IsInPhase(PHASE_DANCE) ? 4s : 10s);
                    break;
                case EVENT_SAFETY_DANCE:
                    CheckSafetyDance();
                    events.Repeat(5s);
                    break;
            }

            if (me->HasUnitState(UNIT_STATE_CASTING))
                return;
        }

        DoMeleeAttackIfReady();
    }

    void CheckSafetyDance()
    {
        if (Map* map = me->GetMap())
        {
            map->DoForAllPlayers([&](Player* p)
            {
                if (IsInBoundary(p) && !p->IsAlive())
                {
                    instance->SetData(DATA_DANCE_FAIL, 0);
                    instance->StorePersistentData(PERSISTENT_DATA_IMMORTAL_FAIL, 1);
                    return;
                }
            });
        }
    }
private:
    uint8 _currentSection{3};
    bool _moveRight{true};
};

void AddSC_boss_heigan()
{
    RegisterNaxxramasCreatureAI(boss_heigan);
}
