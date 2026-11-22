#pragma once
#include <chrono>
#include <iterator>
#include <mutex>
#include <unordered_map>

#include "RE/Skyrim.h"

// Weapon-animation guard.
//
// ActorState::GetWeaponState() flips to kSheathed as soon as the engine starts
// the sheathe transition, while the weaponSheath clip still has a few tenths of
// a second left. Any idle / package change issued in that gap replaces the clip,
// leaves the weapon in the hand with ActorState already reporting "sheathed",
// and the next combat draw never lands (the actor just takes hits).
//
// Every follower-side check that used to trust IsWeaponDrawn() alone now asks
// this header instead: the weapon state has to be settled AND the actor must
// not have left combat within the quiet window.
namespace Wayfarer::Guard
{
    namespace Detail
    {
        inline constexpr std::chrono::milliseconds quietWindow{1200};

        inline std::mutex clockMutex;
        inline std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> combatExitAt;

        inline bool Stale(RE::FormID a_id, std::chrono::steady_clock::time_point a_now)
        {
            const auto it = combatExitAt.find(a_id);
            if (it == combatExitAt.end()) {
                return true;
            }
            if (a_now - it->second >= quietWindow) {
                combatExitAt.erase(it);
                return true;
            }
            return false;
        }
    } // namespace Detail

    inline void NoteCombatEnded(RE::FormID a_id)
    {
        if (!a_id) {
            return;
        }
        std::lock_guard lock(Detail::clockMutex);
        const auto now = std::chrono::steady_clock::now();
        if (Detail::combatExitAt.size() > 64) {
            for (auto it = Detail::combatExitAt.begin(); it != Detail::combatExitAt.end();) {
                it = now - it->second >= Detail::quietWindow ? Detail::combatExitAt.erase(it) : std::next(it);
            }
        }
        Detail::combatExitAt[a_id] = now;
    }

    inline bool InQuietWindow(RE::FormID a_id)
    {
        std::lock_guard lock(Detail::clockMutex);
        return !Detail::Stale(a_id, std::chrono::steady_clock::now());
    }

    // kSheathing / kDrawing are the states where the graph is mid-clip. A null
    // state means no 3D / no runtime data yet: treat it as busy.
    inline bool WeaponAnimating(RE::Actor& a_actor)
    {
        auto* state = a_actor.AsActorState();
        if (!state) {
            return true;
        }
        const auto weapon = state->GetWeaponState();
        return weapon == RE::WEAPON_STATE::kSheathing || weapon == RE::WEAPON_STATE::kDrawing;
    }

    inline bool BlockAnimationChange(RE::Actor& a_actor)
    {
        return WeaponAnimating(a_actor) || InQuietWindow(a_actor.GetFormID());
    }

    inline bool BlockAnimationChange(RE::Actor* a_actor)
    {
        return a_actor && BlockAnimationChange(*a_actor);
    }
} // namespace Wayfarer::Guard
