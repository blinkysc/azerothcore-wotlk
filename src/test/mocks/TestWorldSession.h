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

#ifndef TEST_WORLD_SESSION_H
#define TEST_WORLD_SESSION_H

#include "WorldSession.h"

/**
 * TestWorldSession - A minimal WorldSession for unit testing
 *
 * Provides just enough implementation to construct a Player without crashing.
 */
class TestWorldSession : public WorldSession
{
public:
    TestWorldSession()
        : WorldSession(0,                    // id
                       std::string(""),      // name (as rvalue)
                       0,                    // accountFlags
                       nullptr,              // sock
                       SEC_PLAYER,           // sec
                       2,                    // expansion (WOTLK)
                       0,                    // mute_time
                       LOCALE_enUS,          // locale
                       0,                    // recruiter
                       false,                // isARecruiter
                       false,                // skipQueue
                       0)                    // TotalTime
    {
    }

    ~TestWorldSession() = default;

    // Override methods that might cause issues in tests - SendPacket is not virtual
};

#endif // TEST_WORLD_SESSION_H
