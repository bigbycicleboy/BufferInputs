#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace ib {

    constexpr int   kRapidGapTicks = 8;
    constexpr int   kRapidStreakLimit = 3;
    constexpr int   kNoInputYet = 999;
    constexpr float kDefaultDt = 0.25f;

    constexpr int kEarliestPriority = -1000000;

    constexpr int kWaitIdle = -1;
    constexpr int kWaitFlushOpp = -2;

    constexpr int kHorizonMiniWave = 50;
    constexpr int kHorizonWave = 65;
    constexpr int kHorizonOther = 160;

    constexpr bool kSimPads = true;
    constexpr bool kSimOrbs = true;
    constexpr bool kSimGravityPortals = true;
    constexpr bool kSimSizePortals = true;

    constexpr bool kSimBoundaryPortals = true;

    constexpr bool kSimInteractables =
        kSimPads || kSimOrbs || kSimGravityPortals || kSimSizePortals || kSimBoundaryPortals;

    struct Preset {
        bool        active;
        int         normalTicks;
        int         waveTicks;
        const char* banner;
    };

    constexpr Preset kPresets[] = {
        { true,   2,  2, "[InputBuffer] 2 ticks All | 2 ticks Wave" },
        { true,   4,  4, "[InputBuffer] 4 ticks All | 4 ticks Wave" },
        { true,   8,  8, "[InputBuffer] 8 ticks All | 8 ticks Wave" },
        { true,  16,  8, "[InputBuffer] 16 ticks All | 8 ticks Wave" },
        { true,  24, 16, "[InputBuffer] 24 ticks All | 16 ticks Wave" },
        { true,  32, 24, "[InputBuffer] 32 ticks All | 24 ticks Wave" },
        { false,  0,  0, "[InputBuffer] OFF (normal input)" },
    };
    constexpr int kPresetCount = static_cast<int>(std::size(kPresets));

    template <class Self>
    void runFirst(Self& self, std::initializer_list<const char*> hooks) {
        for (const char* h : hooks) {
            (void)self.setHookPriority(h, kEarliestPriority);
        }
    }

    inline bool isSolidType(GameObjectType t) {
        switch (t) {
        case GameObjectType::Solid:
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
        case GameObjectType::CollisionObject:
        case GameObjectType::Slope:
            return true;
        default:
            return false;
        }
    }

    inline bool isSimInteractable(GameObjectType t) {
        switch (t) {
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::SpiderPad:
            return kSimPads;

        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
            return kSimOrbs;

        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
            return kSimGravityPortals;

        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
            return kSimSizePortals;

        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::TeleportPortal:
            return kSimBoundaryPortals;

        default:
            return false;
        }
    }

    class Engine {
        struct Subject {
            bool a = false;
            bool b = false;
        };

        struct Lane {
            bool userHeld = false;
            bool appliedHeld = false;
            int  countdown = kWaitIdle;
            bool pendingDown = true;
            bool pendingOpp = false;
            int  sinceInput = kNoInputYet;
            int  rapidStreak = 0;
        };

        struct ObjSnap {
            GameObject* obj = nullptr;
            bool isActivated = false;
            bool ringPowered = false;
            bool enh = false;
            bool activated = false;
            bool byP1 = false;
            bool byP2 = false;
            bool animTriggered = false;
        };

        struct GlobalSnap {
            float portalY = 0.f;
            float lastModeHeight = 0.f;
            bool gravityRelated = false;
            GameObject* lastPortal1 = nullptr;
            GameObject* lastPortal2 = nullptr;
            gd::map<std::pair<int, int>, int> activatedIds;
        };

        PlayerObject* m_ghostA = nullptr;
        PlayerObject* m_ghostB = nullptr;

        bool m_inSim = false;
        bool m_injecting = false;
        bool m_ghostDied = false;
        bool m_boundaryHit = false;

        std::vector<ObjSnap> m_objSnaps;
        std::unordered_set<GameObject*> m_seen;
        GlobalSnap m_gSnap;

        Lane m_lane[2];

        int  m_presetIdx = 0;
        bool m_active = true;
        int  m_limitNormal = 2;
        int  m_limitWave = 2;

        float m_lastDt = kDefaultDt;

        bool m_f6Held = false;

        static bool isDual(PlayLayer* pl) {
            return pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        }

        static bool isTwoPlayerLevel(PlayLayer* pl) {
            return pl->m_level && pl->m_level->m_twoPlayerMode;
        }

        static bool isIndependentDual(PlayLayer* pl) {
            return isDual(pl) && isTwoPlayerLevel(pl);
        }

        static bool isSoloDual(PlayLayer* pl) {
            return pl->m_gameState.m_isDualMode && !isTwoPlayerLevel(pl);
        }

        static Subject subjectFor(PlayLayer* pl, int lane) {
            Subject s;
            if (!isDual(pl)) {
                s.a = (lane == 0);
                return s;
            }
            if (isTwoPlayerLevel(pl)) {
                s.a = (lane == 0);
                s.b = (lane == 1);
                return s;
            }
            s.a = true;
            s.b = true;
            return s;
        }

        template <class F>
        static bool anyControlled(const Subject& s, PlayerObject* p1, PlayerObject* p2, F pred) {
            return (s.a && p1 && pred(p1)) || (s.b && p2 && pred(p2));
        }

        static bool laneHasShip(PlayLayer* pl, int lane) {
            const Subject s = subjectFor(pl, lane);
            return anyControlled(s, pl->m_player1, pl->m_player2,
                [](PlayerObject* p) { return p->m_isShip; });
        }

        void dropBuffer(int lane) {
            m_lane[lane].countdown = kWaitIdle;
            m_lane[lane].pendingOpp = false;
        }

        void dropAllBuffers() {
            dropBuffer(0);
            dropBuffer(1);
        }

        void resetInputState() {
            for (Lane& L : m_lane) {
                L.countdown = kWaitIdle;
                L.pendingOpp = false;
                L.userHeld = false;
                L.appliedHeld = false;
                L.sinceInput = kNoInputYet;
                L.rapidStreak = 0;
            }
        }

        PlayerObject* spawnGhost(PlayLayer* pl) {
            if (!pl || !pl->m_objectLayer) return nullptr;

            PlayerObject* g = PlayerObject::create(1, 1, pl, pl, true);
            if (!g) return nullptr;
            g->retain();
            g->setPosition({ 0, 105 });
            g->setVisible(false);
            pl->m_objectLayer->addChild(g);
            return g;
        }

        void freeGhost(PlayerObject*& g) {
            if (!g) return;
            if (g->m_touchingRings) g->m_touchingRings->removeAllObjects();
            g->removeFromParent();
            g->release();
            g = nullptr;
        }

        static void wipeCollisions(PlayerObject* p) {
            p->m_collisionLogTop->removeAllObjects();
            p->m_collisionLogBottom->removeAllObjects();
            p->m_collisionLogLeft->removeAllObjects();
            p->m_collisionLogRight->removeAllObjects();
        }

        static void mirrorPlayer(PlayerObject* dst, PlayerObject* src) {
            dst->copyAttributes(src);
            dst->setPosition(src->getPosition());
            dst->setScale(src->getScale());
            dst->setContentSize(src->getContentSize());
            dst->m_width = src->m_width;
            dst->m_height = src->m_height;
            dst->m_yVelocity = src->m_yVelocity;
            dst->m_gravityMod = src->m_gravityMod;
            dst->m_isOnGround = src->m_isOnGround;
            dst->m_isUpsideDown = src->m_isUpsideDown;
            dst->m_vehicleSize = src->m_vehicleSize;
            dst->m_playerSpeed = src->m_playerSpeed;

            dst->m_isSecondPlayer = src->m_isSecondPlayer;
            dst->m_touchedRings = src->m_touchedRings;
            dst->m_lastActivatedPortal = src->m_lastActivatedPortal;
            dst->m_touchedGravityPortal = src->m_touchedGravityPortal;
            dst->m_lastPortalPos = src->m_lastPortalPos;
            dst->m_stateRingJump = src->m_stateRingJump;
            dst->m_stateRingJump2 = src->m_stateRingJump2;
            dst->m_ringJumpRelated = src->m_ringJumpRelated;
            dst->m_jumpBuffered = src->m_jumpBuffered;
            dst->m_isDashing = src->m_isDashing;
            dst->m_dashStartTime = src->m_dashStartTime;
            dst->m_dashX = src->m_dashX;
            dst->m_dashY = src->m_dashY;
            dst->m_dashRing = src->m_dashRing;

            if (dst->m_touchingRings) {
                dst->m_touchingRings->removeAllObjects();
                if (src->m_touchingRings) {
                    dst->m_touchingRings->addObjectsFromArray(src->m_touchingRings);
                }
            }

            dst->setVisible(false);
        }

        float stepDt() const {
            const bool sane = m_lastDt > 0.001f && m_lastDt < 2.0f;
            return sane ? m_lastDt : kDefaultDt;
        }

        static void setGhostButtons(PlayerObject* const* ghosts, int count, bool press) {
            for (int i = 0; i < count; ++i) {
                if (press) ghosts[i]->pushButton(PlayerButton::Jump);
                else       ghosts[i]->releaseButton(PlayerButton::Jump);
            }
        }

        void injectButton(PlayLayer* pl, int lane, bool down) {
            m_injecting = true;
            m_lane[lane].appliedHeld = down;
            if (lane == 0) {
                pl->handleButton(down, 1, true);
                if (isSoloDual(pl)) {
                    pl->handleButton(down, 1, false);
                }
            }
            else {
                pl->handleButton(down, 1, false);
            }
            m_injecting = false;
        }

        void resyncHeld(PlayLayer* pl, int lane) {
            if (m_lane[lane].appliedHeld != m_lane[lane].userHeld) {
                injectButton(pl, lane, m_lane[lane].userHeld);
            }
        }

        void snapshotWorld(PlayLayer* pl) {
            m_objSnaps.clear();
            m_seen.clear();

            auto& gs = pl->m_gameState;
            m_gSnap.portalY = gs.m_portalY;
            m_gSnap.lastModeHeight = gs.m_lastModeHeight;
            m_gSnap.gravityRelated = gs.m_gravityRelated;
            m_gSnap.lastPortal1 = gs.m_lastActivatedPortal1;
            m_gSnap.lastPortal2 = gs.m_lastActivatedPortal2;
            m_gSnap.activatedIds = gs.m_activatedObjectIDs;
        }

        void restoreWorld(PlayLayer* pl) {
            for (const ObjSnap& s : m_objSnaps) {
                s.obj->m_isActivated = s.isActivated;
                s.obj->m_isRingPoweredOn = s.ringPowered;
                if (s.enh) {
                    auto* e = static_cast<EnhancedGameObject*>(s.obj);
                    e->m_activated = s.activated;
                    e->m_activatedByPlayer1 = s.byP1;
                    e->m_activatedByPlayer2 = s.byP2;
                    e->m_animationTriggered = s.animTriggered;
                }
            }
            m_objSnaps.clear();
            m_seen.clear();

            auto& gs = pl->m_gameState;
            gs.m_portalY = m_gSnap.portalY;
            gs.m_lastModeHeight = m_gSnap.lastModeHeight;
            gs.m_gravityRelated = m_gSnap.gravityRelated;
            gs.m_lastActivatedPortal1 = m_gSnap.lastPortal1;
            gs.m_lastActivatedPortal2 = m_gSnap.lastPortal2;
            gs.m_activatedObjectIDs = m_gSnap.activatedIds;

            if (m_ghostA && m_ghostA->m_touchingRings) m_ghostA->m_touchingRings->removeAllObjects();
            if (m_ghostB && m_ghostB->m_touchingRings) m_ghostB->m_touchingRings->removeAllObjects();
        }

        int lookAhead(PlayLayer* pl, PlayerObject* p1, PlayerObject* p2, const Subject& sub,
            int delay, bool down, int horizon) {
            if (!pl || !p1) return horizon;

            PlayerObject* gA = (sub.a && m_ghostA) ? m_ghostA : nullptr;
            PlayerObject* gB = (sub.b && p2 && m_ghostB) ? m_ghostB : nullptr;
            if (!gA && !gB) return horizon;

            m_inSim = true;
            m_ghostDied = false;
            m_boundaryHit = false;
            snapshotWorld(pl);

            PlayerObject* ghosts[2] = { nullptr, nullptr };
            int ghostCount = 0;
            if (gA) {
                mirrorPlayer(gA, p1);
                ghosts[ghostCount++] = gA;
            }
            if (gB) {
                mirrorPlayer(gB, p2);
                ghosts[ghostCount++] = gB;
            }

            if (delay > 0) {
                setGhostButtons(ghosts, ghostCount, !down);
            }

            const float dt = stepDt();
            int  alive = 0;
            const bool continuous =
                (gA && (p1->m_isDart || p1->m_isBird || p1->m_isSwing)) ||
                (gB && (p2->m_isDart || p2->m_isBird || p2->m_isSwing));
            int inAir = 0;
            int settled = 0;

            const int total = delay + horizon;
            for (int step = 0; step < total; ++step) {
                if (step == delay) {
                    setGhostButtons(ghosts, ghostCount, down);
                }

                for (int i = 0; i < ghostCount; ++i) {
                    ghosts[i]->update(dt);
                    ghosts[i]->updateRotation(dt);
                }
                if (m_ghostDied) break;

                for (int i = 0; i < ghostCount; ++i) {
                    wipeCollisions(ghosts[i]);
                    pl->checkCollisions(ghosts[i], dt, false);
                    if (m_ghostDied || m_boundaryHit) break;
                }
                if (m_ghostDied || m_boundaryHit) break;

                if (step < delay) continue;

                ++alive;
                if (continuous) continue;

                const bool aGround = gA ? gA->m_isOnGround : true;
                const bool bGround = gB ? gB->m_isOnGround : true;
                if (!aGround || !bGround) {
                    ++inAir;
                    settled = 0;
                }
                else if (inAir >= 15) {
                    if (++settled >= 10) {
                        alive = horizon;
                        break;
                    }
                }
            }

            if (m_boundaryHit && !m_ghostDied) {
                alive = horizon;
            }

            restoreWorld(pl);
            m_inSim = false;
            return alive;
        }

        bool filterLane(GJBaseGameLayer* layer, PlayLayer* pl, int lane, bool down, bool dual) {
            Lane& L = m_lane[lane];
            L.userHeld = down;

            if (!m_active) {
                L.appliedHeld = down;
                return true;
            }

            const Subject sub = subjectFor(pl, lane);
            PlayerObject* p1 = layer->m_player1;
            PlayerObject* p2 = dual ? pl->m_player2 : nullptr;
            PlayerObject* lead = sub.a ? p1 : p2;

            if (!lead || lead->m_isDead) {
                dropBuffer(lane);
                L.appliedHeld = down;
                return true;
            }

            if (anyControlled(sub, p1, p2, [](PlayerObject* p) { return p->m_isShip; })) {
                dropBuffer(lane);
                L.appliedHeld = down;
                return true;
            }

            if (L.countdown >= 0) {
                L.pendingOpp = (down != L.pendingDown);
                L.sinceInput = 0;
                return false;
            }

            if (L.countdown == kWaitFlushOpp) {
                dropBuffer(lane);
                const bool opp = !L.pendingDown;
                if (L.appliedHeld != opp) {
                    injectButton(pl, lane, opp);
                }
            }

            const bool wave = anyControlled(sub, p1, p2,
                [](PlayerObject* p) { return p->m_isDart; });
            const bool miniWave = anyControlled(sub, p1, p2,
                [](PlayerObject* p) { return p->m_isDart && p->m_vehicleSize < 1.0f; });

            L.rapidStreak = (L.sinceInput < kRapidGapTicks) ? L.rapidStreak + 1 : 0;
            L.sinceInput = 0;

            if (wave && L.rapidStreak >= kRapidStreakLimit) {
                L.appliedHeld = down;
                return true;
            }

            const bool releaseMatters = anyControlled(sub, p1, p2,
                [](PlayerObject* p) { return p->m_isDart || p->m_isRobot; });
            if (!down && !releaseMatters) {
                L.appliedHeld = false;
                return true;
            }

            L.pendingOpp = false;
            const char* verb = down ? "PRESS" : "RELEASE";
            const char* who = (sub.a && sub.b) ? "P1+P2" : (sub.a ? "P1" : "P2");

            const int horizon = miniWave ? kHorizonMiniWave : (wave ? kHorizonWave : kHorizonOther);
            const int maxDelay = wave ? m_limitWave : m_limitNormal;

            const int base = lookAhead(pl, p1, p2, sub, 0, down, horizon);
            if (base >= horizon) {
                L.appliedHeld = down;
                return true;
            }

            int pickedDelay = 0;
            int pickedTicks = base;

            auto take = [&](int d, int t) { pickedDelay = d; pickedTicks = t; };

            for (int d = 1; d <= maxDelay; ++d) {
                const int t = lookAhead(pl, p1, p2, sub, d, down, horizon);

                if (wave) {
                    const int needed = d * 2 + 10;
                    if (t >= horizon && (t - base) >= d * 2 + 6) {
                        take(d, t);
                        break;
                    }
                    if (t >= base + needed && t > pickedTicks + 2) {
                        take(d, t);
                        if (t >= horizon) break;
                    }
                }
                else {
                    if (t >= horizon) {
                        take(d, t);
                        break;
                    }
                    if (t > pickedTicks + 12) {
                        take(d, t);
                    }
                }
            }

            if (pickedDelay > 0) {
                log::info("[InputBuffer] SAVED +{} ticks (~{:.2f} frames @ 240 TPS) on {} {}! (Frame0: {} -> Delayed: {}) [Limit: {}]",
                    pickedDelay, pickedDelay / 4.0f, who, verb, base, pickedTicks, maxDelay);
                L.pendingDown = down;
                L.countdown = pickedDelay;
                return false;
            }

            L.appliedHeld = down;
            return true;
        }

        void tickLane(PlayLayer* pl, int lane) {
            Lane& L = m_lane[lane];

            if (L.sinceInput < kNoInputYet) ++L.sinceInput;

            if (laneHasShip(pl, lane)) {
                dropBuffer(lane);
                resyncHeld(pl, lane);
                return;
            }

            if (L.countdown > 0) {
                --L.countdown;
                return;
            }

            if (L.countdown == 0) {
                injectButton(pl, lane, L.pendingDown);
                const bool follow = L.pendingOpp;
                L.pendingOpp = false;
                L.countdown = follow ? kWaitFlushOpp : kWaitIdle;
                return;
            }

            if (L.countdown == kWaitFlushOpp) {
                injectButton(pl, lane, !L.pendingDown);
                L.pendingOpp = false;
                L.countdown = kWaitIdle;
                return;
            }

            resyncHeld(pl, lane);
        }

        void pollHotkey() {
#ifdef GEODE_IS_WINDOWS
            const bool pressed = GetActiveWindow() != nullptr &&
                (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
            if (pressed && !m_f6Held) {
                nextPreset();
            }
            m_f6Held = pressed;
#endif
        }

    public:
        void begin(PlayLayer* pl) {
            end();

            if (!pl) return;

            m_ghostA = spawnGhost(pl);
            m_ghostB = spawnGhost(pl);
            m_inSim = false;
            m_injecting = false;
            resetInputState();
        }

        void end() {
            freeGhost(m_ghostA);
            freeGhost(m_ghostB);
            m_inSim = false;
            m_injecting = false;
            m_objSnaps.clear();
            m_seen.clear();
            resetInputState();
        }

        void onReset() {
            resetInputState();
        }

        void nextPreset() {
            m_presetIdx = (m_presetIdx + 1) % kPresetCount;
            dropAllBuffers();

            const Preset& p = kPresets[m_presetIdx];
            m_active = p.active;
            m_limitNormal = p.normalTicks;
            m_limitWave = p.waveTicks;
            log::info("{}", p.banner);
        }

        bool simActive() const { return m_inSim; }
        bool injecting() const { return m_injecting; }

        void noteBoundary() { m_boundaryHit = true; }

        void recordObject(GameObject* o) {
            if (!o || !m_seen.insert(o).second) return;

            ObjSnap s;
            s.obj = o;
            s.isActivated = o->m_isActivated;
            s.ringPowered = o->m_isRingPoweredOn;
            if (auto* e = typeinfo_cast<EnhancedGameObject*>(o)) {
                s.enh = true;
                s.activated = e->m_activated;
                s.byP1 = e->m_activatedByPlayer1;
                s.byP2 = e->m_activatedByPlayer2;
                s.animTriggered = e->m_animationTriggered;
            }
            m_objSnaps.push_back(s);
        }

        bool notifyDeath(PlayerObject* who) {
            if (m_inSim && who && (who == m_ghostA || who == m_ghostB)) {
                m_ghostDied = true;
                return true;
            }
            resetInputState();
            return false;
        }

        void trackFrameDelta(float dt) {
            PlayLayer* pl = PlayLayer::get();
            const float warp = pl ? pl->m_gameState.m_timeWarp : 0.f;
            m_lastDt = (pl && warp > 0.f) ? dt / warp : dt;
        }

        bool filterInput(GJBaseGameLayer* layer, bool down, bool isP1) {
            PlayLayer* pl = PlayLayer::get();
            if (!pl) return true;

            const bool dual = isDual(pl);
            const bool twoCtl = isTwoPlayerLevel(pl);
            const bool soloDual = dual && !twoCtl;
            const bool indepDual = dual && twoCtl;

            if (!isP1) {
                if (indepDual) {
                    return filterLane(layer, pl, 1, down, dual);
                }
                return !(soloDual && m_active);
            }

            const bool pass = filterLane(layer, pl, 0, down, dual);

            if (pass && soloDual && m_active) {
                m_injecting = true;
                pl->handleButton(down, 1, false);
                m_injecting = false;
            }
            return pass;
        }

        void tick(PlayerObject* who) {
            PlayLayer* pl = PlayLayer::get();
            if (!pl || who != pl->m_player1) return;

            pollHotkey();

            if (!m_active) return;

            tickLane(pl, 0);

            if (isIndependentDual(pl)) {
                tickLane(pl, 1);
            }
            else if (m_lane[1].countdown != kWaitIdle) {
                dropBuffer(1);
            }
        }
    };

    static Engine g_engine;

}

$on_mod(Loaded) {
    log::info("[InputBuffer] v1.2.0 loaded!");
}

class $modify(IBPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        ib::runFirst(self, {
            "PlayLayer::destroyPlayer",
            "PlayLayer::playEndAnimationToPos",
            "PlayLayer::playGravityEffect",
            });
    }

    bool init(GJGameLevel * lvl, bool replay, bool noObjects) {
        if (!PlayLayer::init(lvl, replay, noObjects)) return false;
        ib::g_engine.begin(this);
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        ib::g_engine.onReset();
    }

    void destroyPlayer(PlayerObject * who, GameObject * cause) {
        if (ib::g_engine.notifyDeath(who)) return;
        PlayLayer::destroyPlayer(who, cause);
    }

    void playEndAnimationToPos(cocos2d::CCPoint pos) {
        if (ib::g_engine.simActive()) return;
        PlayLayer::playEndAnimationToPos(pos);
    }

    void playGravityEffect(bool flip) {
        if (ib::g_engine.simActive()) return;
        PlayLayer::playGravityEffect(flip);
    }

    void onQuit() {
        PlayLayer::onQuit();
        ib::g_engine.end();
    }
};

class $modify(IBHardStreak, HardStreak) {
    static void onModify(auto& self) {
        ib::runFirst(self, { "HardStreak::addPoint" });
    }

    void addPoint(cocos2d::CCPoint pt) {
        if (ib::g_engine.simActive()) return;
        HardStreak::addPoint(pt);
    }
};

class $modify(IBEffectObject, EffectGameObject) {
    static void onModify(auto& self) {
        ib::runFirst(self, { "EffectGameObject::triggerObject" });
    }

    void triggerObject(GJBaseGameLayer * layer, int id, gd::vector<int> const* ids) {
        if (ib::g_engine.simActive()) return;
        EffectGameObject::triggerObject(layer, id, ids);
    }
};

class $modify(IBGameObject, GameObject) {
    static void onModify(auto& self) {
        ib::runFirst(self, { "GameObject::playShineEffect" });
    }

    void playShineEffect() {
        if (ib::g_engine.simActive()) return;
        GameObject::playShineEffect();
    }
};

class $modify(IBRingObject, RingObject) {
    static void onModify(auto& self) {
        ib::runFirst(self, { "RingObject::spawnCircle" });
    }

    void spawnCircle() {
        if (ib::g_engine.simActive()) return;
        RingObject::spawnCircle();
    }
};

class $modify(IBPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        ib::runFirst(self, {
            "PlayerObject::playSpiderDashEffect",
            "PlayerObject::incrementJumps",
            "PlayerObject::ringJump",
            "PlayerObject::spawnCircle",
            "PlayerObject::spawnPortalCircle",
            });
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (ib::g_engine.simActive()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (ib::g_engine.simActive()) return;
        PlayerObject::incrementJumps();
    }

    void spawnCircle() {
        if (ib::g_engine.simActive()) return;
        PlayerObject::spawnCircle();
    }

    void spawnPortalCircle(cocos2d::ccColor3B color, float startRadius) {
        if (ib::g_engine.simActive()) return;
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void update(float dt) {
        const bool real = !ib::g_engine.simActive();
        if (real) {
            ib::g_engine.tick(this);
        }
        PlayerObject::update(dt);
        if (real && PlayLayer::get()) {
            ib::g_engine.trackFrameDelta(dt);
        }
    }

    void ringJump(RingObject * ring, bool flag) {
        if (ib::g_engine.simActive() && !ib::kSimInteractables) return;
        PlayerObject::ringJump(ring, flag);
    }
};

class $modify(IBBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        ib::runFirst(self, {
            "GJBaseGameLayer::canBeActivatedByPlayer",
            "GJBaseGameLayer::handleButton",
            "GJBaseGameLayer::collisionCheckObjects",
            "GJBaseGameLayer::playerTouchedRing",
            "GJBaseGameLayer::flipGravity",
            "GJBaseGameLayer::gameEventTriggered",
            "GJBaseGameLayer::playerWillSwitchMode",
            "GJBaseGameLayer::toggleDualMode",
            "GJBaseGameLayer::toggleFlipped",
            "GJBaseGameLayer::teleportPlayer",
            });
    }

    bool canBeActivatedByPlayer(PlayerObject * who, EffectGameObject * obj) {
        if (ib::g_engine.simActive() && !ib::kSimInteractables) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(who, obj);
    }

    void handleButton(bool down, int button, bool isP1) {
        const bool intercept = !ib::g_engine.simActive() &&
            !ib::g_engine.injecting() &&
            button == 1 &&
            PlayLayer::get();
        if (intercept && !ib::g_engine.filterInput(this, down, isP1)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isP1);
    }

    void collisionCheckObjects(PlayerObject * who, gd::vector<GameObject*>*list, int count, float dt) {
        if (!ib::g_engine.simActive() || !list) {
            GJBaseGameLayer::collisionCheckObjects(who, list, count, dt);
            return;
        }

        static gd::vector<GameObject*> filtered;
        filtered.clear();
        for (int i = 0; i < count; ++i) {
            GameObject* o = (*list)[i];
            if (!o) continue;

            const GameObjectType t = o->m_objectType;
            if (ib::isSolidType(t)) {
                filtered.push_back(o);
            }
            else if (ib::isSimInteractable(t)) {
                ib::g_engine.recordObject(o);
                filtered.push_back(o);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(who, &filtered, static_cast<int>(filtered.size()), dt);
    }

    void playerTouchedRing(PlayerObject * who, RingObject * ring) {
        if (ib::g_engine.simActive() && !ib::kSimInteractables) return;
        GJBaseGameLayer::playerTouchedRing(who, ring);
    }

    void flipGravity(PlayerObject * who, bool flip, bool noEffects) {
        if (ib::g_engine.simActive()) {
            GJBaseGameLayer::flipGravity(who, flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(who, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (ib::g_engine.simActive()) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void playerWillSwitchMode(PlayerObject * who, GameObject * obj) {
        if (ib::g_engine.simActive()) {
            ib::g_engine.noteBoundary();
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(who, obj);
    }

    void toggleDualMode(GameObject * obj, bool dual, PlayerObject * who, bool noEffects) {
        if (ib::g_engine.simActive()) {
            ib::g_engine.noteBoundary();
            return;
        }
        GJBaseGameLayer::toggleDualMode(obj, dual, who, noEffects);
    }

    void toggleFlipped(bool flip, bool noEffects) {
        if (ib::g_engine.simActive()) {
            ib::g_engine.noteBoundary();
            return;
        }
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void teleportPlayer(TeleportPortalObject * obj, PlayerObject * who) {
        if (ib::g_engine.simActive()) {
            ib::g_engine.noteBoundary();
            return;
        }
        GJBaseGameLayer::teleportPlayer(obj, who);
    }
};