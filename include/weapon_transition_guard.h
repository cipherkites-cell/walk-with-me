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
// a second left. Anything that replaces that clip (IdleStop, a pose idle, a
// package re-evaluation) leaves the weapon in the hand with ActorState already
// reporting "sheathed"; the graph then stays in the sheathe pose and the next
// combat draw never lands, so the follower can only take hits.
//
// Two windows have to be respected:
//   1. the weapon state is kSheathing / kDrawing (mid-clip),
//   2. the actor left combat less than quietWindow ago (clip about to start).
// Both are time-bounded, and a transition that never resolves is force-released
// after transitionTimeout so a stuck state can never freeze follower behaviour.
namespace Wayfarer::Guard
{
    namespace Detail
    {
        inline constexpr std::chrono::milliseconds quietWindow{1200};
        inline constexpr std::chrono::milliseconds transitionTimeout{3000};

        inline std::mutex clockMutex;
        inline std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> combatExitAt;
        inline std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> transitionSince;
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
        const auto it = Detail::combatExitAt.find(a_id);
        if (it == Detail::combatExitAt.end()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - it->second >= Detail::quietWindow) {
            Detail::combatExitAt.erase(it);
            return false;
        }
        return true;
    }

    // kSheathing / kDrawing means the graph is mid-clip. A null state means no
    // runtime data yet, which is not a transition: report false so callers never
    // get stuck behind a missing actor state.
    inline bool WeaponAnimating(RE::Actor& a_actor)
    {
        auto* state = a_actor.AsActorState();
        if (!state) {
            return false;
        }
        const auto weapon = state->GetWeaponState();
        const bool busy = weapon == RE::WEAPON_STATE::kSheathing || weapon == RE::WEAPON_STATE::kDrawing;
        const auto id = a_actor.GetFormID();

        std::lock_guard lock(Detail::clockMutex);
        const auto now = std::chrono::steady_clock::now();
        if (!busy) {
            Detail::transitionSince.erase(id);
            return false;
        }
        const auto it = Detail::transitionSince.find(id);
        if (it == Detail::transitionSince.end()) {
            Detail::transitionSince[id] = now;
            return true;
        }
        // A transition that has been mid-flight for seconds is a stuck graph,
        // not an animation that still deserves protection: release it and let
        // the follower logic recover.
        return now - it->second < Detail::transitionTimeout;
    }

    // ---- stuck-weapon detection --------------------------------------------
    // Engine side desync: ActorState reports kSheathed while the animation graph
    // still believes the weapon is out (iWeaponState != 0 / bWeaponSheathed false).
    // The weapon stays attached to the hand, the follower can no longer draw,
    // and only an unequip/re-equip cycle clears it - exactly what a player does
    // by hand. Snapshot below reports what each signal actually says.
    struct Snapshot
    {
        std::int32_t actorState{-1};
        std::int32_t graphState{-1};
        float graphStateFloat{-1.0F};
        bool graphSheathed{true};
        bool hasGraphState{false};
        bool hasGraphSheathed{false};
        bool hasWeapon{false};
    };

    inline Snapshot TakeSnapshot(RE::Actor& a_actor)
    {
        Snapshot snap;
        if (auto* state = a_actor.AsActorState()) {
            snap.actorState = static_cast<std::int32_t>(state->GetWeaponState());
        }
        snap.hasGraphState = a_actor.GetGraphVariableInt("iWeaponState", snap.graphState);
        if (!snap.hasGraphState) {
            snap.hasGraphState = a_actor.GetGraphVariableFloat("iWeaponState", snap.graphStateFloat);
            if (snap.hasGraphState) {
                snap.graphState = static_cast<std::int32_t>(snap.graphStateFloat);
            }
        }
        snap.hasGraphSheathed = a_actor.GetGraphVariableBool("bWeaponSheathed", snap.graphSheathed);
        snap.hasWeapon = a_actor.GetEquippedObject(false) != nullptr || a_actor.GetEquippedObject(true) != nullptr;
        return snap;
    }

    inline bool Desynced(const Snapshot& a_snap)
    {
        if (a_snap.actorState != static_cast<std::int32_t>(RE::WEAPON_STATE::kSheathed)) {
            return false;
        }
        if (!a_snap.hasWeapon) {
            return false;
        }
        if (a_snap.hasGraphState && a_snap.graphState != 0) {
            return true;
        }
        if (a_snap.hasGraphSheathed && !a_snap.graphSheathed) {
            return true;
        }
        return false;
    }

    enum class RepairAction
    {
        kNone,
        kStrip,
        kRestore
    };

    namespace Detail
    {
        inline constexpr std::chrono::milliseconds repairDelay{1200};
        inline constexpr std::chrono::milliseconds stripHold{350};
        inline constexpr std::chrono::seconds repairCooldown{10};

        struct Repair
        {
            std::chrono::steady_clock::time_point firstSeen{};
            std::chrono::steady_clock::time_point strippedAt{};
            std::chrono::steady_clock::time_point cooldownUntil{};
            RE::FormID weapon{0};
            bool stripped{false};
        };

        inline std::unordered_map<RE::FormID, Repair> repairs;
    } // namespace Detail

    // Returns kStrip once the desync has lasted repairDelay, then kRestore after
    // the weapon has been off for stripHold. a_weapon carries the weapon FormID
    // for the restore call. Any desync that clears by itself never gets touched.
    inline RepairAction StepRepair(RE::Actor& a_actor, RE::FormID& a_weapon)
    {
        a_weapon = 0;
        const auto now = std::chrono::steady_clock::now();
        const auto id = a_actor.GetFormID();
        const auto snap = TakeSnapshot(a_actor);

        std::lock_guard lock(Detail::clockMutex);
        const auto it = Detail::repairs.find(id);
        const bool desynced = Desynced(snap) && !a_actor.IsInCombat();

        if (!desynced) {
            if (it != Detail::repairs.end() && it->second.stripped) {
                if (now - it->second.strippedAt < Detail::stripHold) {
                    return RepairAction::kNone;
                }
                a_weapon = it->second.weapon;
                it->second.stripped = false;
                it->second.cooldownUntil = now + Detail::repairCooldown;
                return RepairAction::kRestore;
            }
            if (it != Detail::repairs.end()) {
                Detail::repairs.erase(it);
            }
            return RepairAction::kNone;
        }

        if (it == Detail::repairs.end()) {
            Detail::Repair fresh;
            fresh.firstSeen = now;
            Detail::repairs[id] = fresh;
            return RepairAction::kNone;
        }

        auto& repair = it->second;
        if (now < repair.cooldownUntil) {
            return RepairAction::kNone;
        }
        if (!repair.stripped) {
            if (now - repair.firstSeen < Detail::repairDelay) {
                return RepairAction::kNone;
            }
            auto* form = a_actor.GetEquippedObject(false);
            if (!form) {
                form = a_actor.GetEquippedObject(true);
            }
            if (!form) {
                Detail::repairs.erase(it);
                return RepairAction::kNone;
            }
            repair.weapon = form->GetFormID();
            repair.stripped = true;
            repair.strippedAt = now;
            return RepairAction::kStrip;
        }
        if (now - repair.strippedAt < Detail::stripHold) {
            return RepairAction::kNone;
        }
        a_weapon = repair.weapon;
        repair.stripped = false;
        repair.cooldownUntil = now + Detail::repairCooldown;
        return RepairAction::kRestore;
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
