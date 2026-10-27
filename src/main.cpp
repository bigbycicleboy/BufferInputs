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

namespace ibuf
{
    constexpr int kSpamGapFrames = 8;
    constexpr int kSpamStreakCap = 3;
    constexpr int kNoInputMark = 999;
    constexpr float kFallbackDt = 0.25f;

    constexpr int kTopPriority = -1000000;

    constexpr int kIdleState = -1;
    constexpr int kFlushOppState = -2;

    constexpr int kRangeMiniWave = 50;
    constexpr int kRangeWave = 65;
    constexpr int kRangeOther = 160;

    constexpr bool kAllowPads = true;
    constexpr bool kAllowOrbs = true;
    constexpr bool kAllowGravityPortals = true;
    constexpr bool kAllowSizePortals = true;

    constexpr bool kAllowBoundaryPortals = true;

    constexpr bool kAllowInteractables =
        kAllowPads || kAllowOrbs || kAllowGravityPortals || kAllowSizePortals || kAllowBoundaryPortals;

    struct ModeConfig
    {
        bool enabled;
        int  cubeDelay;
        int  waveDelay;
        const char* label;
    };

    constexpr ModeConfig kModeTable[] =
    {
        { true,   2,  2, "[BufferInputs] (2 ticks All | 2 ticks Wave)" },
        { true,   4,  4, "[BufferInputs] (4 ticks All | 4 ticks Wave)" },
        { true,   8,  8, "[BufferInputs] (8 ticks All | 8 ticks Wave)" },
        { true,  16,  8, "[BufferInputs] (16 ticks All | 8 ticks Wave)" },
        { false,  0,  0, "[BufferInputs] (OFF)" },
    };
    constexpr int kModeCount = static_cast<int>(std::size(kModeTable));

    template <class Self>
    void hookFirst(Self& self, std::initializer_list<const char*> hooks)
    {
        for (const char* h : hooks)
        {
            (void)self.setHookPriority(h, kTopPriority);
        }
    }

    inline bool isBlocking(GameObjectType t)
    {
        switch (t)
        {
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

    inline bool isTrackable(GameObjectType t)
    {
        switch (t)
        {
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::SpiderPad:
            return kAllowPads;

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
            return kAllowOrbs;

        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
            return kAllowGravityPortals;

        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
            return kAllowSizePortals;

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
            return kAllowBoundaryPortals;

        default:
            return false;
        }
    }

    class Predictor
    {
        Ref<Notification> m_notif;

        struct Controlled
        {
            bool drivesP1 = false;
            bool drivesP2 = false;
        };

        struct InputLane
        {
            bool heldByUser = false;
            bool heldApplied = false;
            int  delayCounter = kIdleState;
            bool queuedDown = true;
            bool queuedOpposite = false;
            int  framesSinceInput = kNoInputMark;
            int  spamStreak = 0;
        };

        struct ObjectState
        {
            GameObject* target = nullptr;
            bool wasActivated = false;
            bool wasRingPowered = false;
            bool isEnhanced = false;
            bool wasTriggered = false;
            bool triggeredByP1 = false;
            bool triggeredByP2 = false;
            bool wasAnimTriggered = false;
        };

        struct SceneState
        {
            float portalY = 0.f;
            float modeHeight = 0.f;
            bool gravityFlag = false;
            GameObject* portal1 = nullptr;
            GameObject* portal2 = nullptr;
            gd::map<std::pair<int, int>, int> activatedIds;
        };

        PlayerObject* m_shadowP1 = nullptr;
        PlayerObject* m_shadowP2 = nullptr;

        bool m_simRunning = false;
        bool m_isInjecting = false;
        bool m_shadowDied = false;
        bool m_hitBoundary = false;

        std::vector<ObjectState> m_stateLog;
        std::unordered_set<GameObject*> m_seenSet;
        SceneState m_sceneSnap;

        InputLane m_lanes[2];

        int  m_modeIdx = 0;
        bool m_enabled = true;
        int  m_cubeDelayLimit = 2;
        int  m_waveDelayLimit = 2;

        float m_cachedDt = kFallbackDt;

        bool m_hotkeyHeld = false;

        static bool isDualMode(PlayLayer* pl)
        {
            return pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        }

        static bool hasSplitControls(PlayLayer* pl)
        {
            return pl->m_level && pl->m_level->m_twoPlayerMode;
        }

        static bool isSplitDual(PlayLayer* pl)
        {
            return isDualMode(pl) && hasSplitControls(pl);
        }

        static bool isMergedDual(PlayLayer* pl)
        {
            return pl->m_gameState.m_isDualMode && !hasSplitControls(pl);
        }

        static Controlled controlledFor(PlayLayer* pl, int lane)
        {
            Controlled c;
            if (!isDualMode(pl))
            {
                c.drivesP1 = (lane == 0);
                return c;
            }
            if (hasSplitControls(pl))
            {
                c.drivesP1 = (lane == 0);
                c.drivesP2 = (lane == 1);
                return c;
            }
            c.drivesP1 = true;
            c.drivesP2 = true;
            return c;
        }

        template <class F>
        static bool anyMatches(const Controlled& c, PlayerObject* p1, PlayerObject* p2, F pred)
        {
            return (c.drivesP1 && p1 && pred(p1)) || (c.drivesP2 && p2 && pred(p2));
        }

        static bool laneIsShip(PlayLayer* pl, int lane)
        {
            const Controlled c = controlledFor(pl, lane);
            return anyMatches(c, pl->m_player1, pl->m_player2,
                [](PlayerObject* p) { return p->m_isShip; });
        }

        void clearLane(int lane)
        {
            m_lanes[lane].delayCounter = kIdleState;
            m_lanes[lane].queuedOpposite = false;
        }

        void clearAllLanes()
        {
            clearLane(0);
            clearLane(1);
        }

        void resetLanes()
        {
            for (InputLane& L : m_lanes)
            {
                L.delayCounter = kIdleState;
                L.queuedOpposite = false;
                L.heldByUser = false;
                L.heldApplied = false;
                L.framesSinceInput = kNoInputMark;
                L.spamStreak = 0;
            }
        }

        PlayerObject* makeShadow(PlayLayer* pl)
        {
            if (!pl || !pl->m_objectLayer) return nullptr;

            PlayerObject* g = PlayerObject::create(1, 1, pl, pl, true);
            if (!g) return nullptr;
            g->retain();
            g->setPosition({ 0, 105 });
            g->setVisible(false);
            pl->m_objectLayer->addChild(g);
            return g;
        }

        void destroyShadow(PlayerObject*& g)
        {
            if (!g) return;
            if (g->m_touchingRings) g->m_touchingRings->removeAllObjects();
            g->removeFromParent();
            g->release();
            g = nullptr;
        }

        static void clearCollisionLog(PlayerObject* p)
        {
            p->m_collisionLogTop->removeAllObjects();
            p->m_collisionLogBottom->removeAllObjects();
            p->m_collisionLogLeft->removeAllObjects();
            p->m_collisionLogRight->removeAllObjects();
        }

        static void copyIntoShadow(PlayerObject* dst, PlayerObject* src)
        {
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

            if (dst->m_touchingRings)
            {
                dst->m_touchingRings->removeAllObjects();
                if (src->m_touchingRings)
                {
                    dst->m_touchingRings->addObjectsFromArray(src->m_touchingRings);
                }
            }

            dst->setVisible(false);
        }

        float currentDt() const
        {
            const bool sane = m_cachedDt > 0.001f && m_cachedDt < 2.0f;
            return sane ? m_cachedDt : kFallbackDt;
        }

        static void pressShadowButtons(PlayerObject* const* shadows, int count, bool press)
        {
            for (int i = 0; i < count; ++i)
            {
                if (press) shadows[i]->pushButton(PlayerButton::Jump);
                else       shadows[i]->releaseButton(PlayerButton::Jump);
            }
        }

        void sendButton(PlayLayer* pl, int lane, bool down)
        {
            m_isInjecting = true;
            m_lanes[lane].heldApplied = down;
            if (lane == 0)
            {
                pl->handleButton(down, 1, true);
                if (isMergedDual(pl))
                {
                    pl->handleButton(down, 1, false);
                }
            }
            else
            {
                pl->handleButton(down, 1, false);
            }
            m_isInjecting = false;
        }

        void syncHeld(PlayLayer* pl, int lane)
        {
            if (m_lanes[lane].heldApplied != m_lanes[lane].heldByUser)
            {
                sendButton(pl, lane, m_lanes[lane].heldByUser);
            }
        }

        void saveScene(PlayLayer* pl)
        {
            m_stateLog.clear();
            m_seenSet.clear();

            auto& gs = pl->m_gameState;
            m_sceneSnap.portalY = gs.m_portalY;
            m_sceneSnap.modeHeight = gs.m_lastModeHeight;
            m_sceneSnap.gravityFlag = gs.m_gravityRelated;
            m_sceneSnap.portal1 = gs.m_lastActivatedPortal1;
            m_sceneSnap.portal2 = gs.m_lastActivatedPortal2;
            m_sceneSnap.activatedIds = gs.m_activatedObjectIDs;
        }

        void loadScene(PlayLayer* pl)
        {
            for (const ObjectState& s : m_stateLog)
            {
                s.target->m_isActivated = s.wasActivated;
                s.target->m_isRingPoweredOn = s.wasRingPowered;
                if (s.isEnhanced)
                {
                    auto* e = static_cast<EnhancedGameObject*>(s.target);
                    e->m_activated = s.wasTriggered;
                    e->m_activatedByPlayer1 = s.triggeredByP1;
                    e->m_activatedByPlayer2 = s.triggeredByP2;
                    e->m_animationTriggered = s.wasAnimTriggered;
                }
            }
            m_stateLog.clear();
            m_seenSet.clear();

            auto& gs = pl->m_gameState;
            gs.m_portalY = m_sceneSnap.portalY;
            gs.m_lastModeHeight = m_sceneSnap.modeHeight;
            gs.m_gravityRelated = m_sceneSnap.gravityFlag;
            gs.m_lastActivatedPortal1 = m_sceneSnap.portal1;
            gs.m_lastActivatedPortal2 = m_sceneSnap.portal2;
            gs.m_activatedObjectIDs = m_sceneSnap.activatedIds;

            if (m_shadowP1 && m_shadowP1->m_touchingRings) m_shadowP1->m_touchingRings->removeAllObjects();
            if (m_shadowP2 && m_shadowP2->m_touchingRings) m_shadowP2->m_touchingRings->removeAllObjects();
        }

        int simulateAhead(PlayLayer* pl, PlayerObject* p1, PlayerObject* p2, const Controlled& ctl,
            int delay, bool down, int range)
        {
            if (!pl || !p1) return range;

            PlayerObject* sA = (ctl.drivesP1 && m_shadowP1) ? m_shadowP1 : nullptr;
            PlayerObject* sB = (ctl.drivesP2 && p2 && m_shadowP2) ? m_shadowP2 : nullptr;
            if (!sA && !sB) return range;

            m_simRunning = true;
            m_shadowDied = false;
            m_hitBoundary = false;
            saveScene(pl);

            PlayerObject* shadows[2] = { nullptr, nullptr };
            int shadowCount = 0;
            if (sA)
            {
                copyIntoShadow(sA, p1);
                shadows[shadowCount++] = sA;
            }
            if (sB)
            {
                copyIntoShadow(sB, p2);
                shadows[shadowCount++] = sB;
            }

            PlayerObject* const realP1 = pl->m_player1;
            PlayerObject* const realP2 = pl->m_player2;
            if (sA) pl->m_player1 = sA;
            if (sB) pl->m_player2 = sB;

            if (delay > 0)
            {
                pressShadowButtons(shadows, shadowCount, !down);
            }

            const float dt = currentDt();
            int  survivedFrames = 0;
            const bool noGroundNeeded =
                (sA && (p1->m_isDart || p1->m_isBird || p1->m_isSwing)) ||
                (sB && (p2->m_isDart || p2->m_isBird || p2->m_isSwing));
            int airFrames = 0;
            int groundedFrames = 0;

            const int total = delay + range;
            for (int step = 0; step < total; ++step)
            {
                if (step == delay)
                {
                    pressShadowButtons(shadows, shadowCount, down);
                }

                for (int i = 0; i < shadowCount; ++i)
                {
                    shadows[i]->update(dt);
                    shadows[i]->updateRotation(dt);
                }
                if (m_shadowDied) break;

                for (int i = 0; i < shadowCount; ++i)
                {
                    clearCollisionLog(shadows[i]);
                    pl->checkCollisions(shadows[i], dt, false);
                    if (m_shadowDied || m_hitBoundary) break;
                }
                if (m_shadowDied || m_hitBoundary) break;

                if (step < delay) continue;

                ++survivedFrames;
                if (noGroundNeeded) continue;

                const bool aGround = sA ? sA->m_isOnGround : true;
                const bool bGround = sB ? sB->m_isOnGround : true;
                if (!aGround || !bGround)
                {
                    ++airFrames;
                    groundedFrames = 0;
                }
                else if (airFrames >= 15)
                {
                    if (++groundedFrames >= 10)
                    {
                        survivedFrames = range;
                        break;
                    }
                }
            }

            if (m_hitBoundary && !m_shadowDied)
            {
                survivedFrames = range;
            }

            pl->m_player1 = realP1;
            pl->m_player2 = realP2;

            loadScene(pl);
            m_simRunning = false;
            return survivedFrames;
        }

        bool processLane(GJBaseGameLayer* layer, PlayLayer* pl, int lane, bool down, bool dual)
        {
            InputLane& L = m_lanes[lane];
            L.heldByUser = down;

            if (!m_enabled)
            {
                L.heldApplied = down;
                return true;
            }

            const Controlled ctl = controlledFor(pl, lane);
            PlayerObject* p1 = layer->m_player1;
            PlayerObject* p2 = dual ? pl->m_player2 : nullptr;
            PlayerObject* lead = ctl.drivesP1 ? p1 : p2;

            if (!lead || lead->m_isDead)
            {
                clearLane(lane);
                L.heldApplied = down;
                return true;
            }

            if (anyMatches(ctl, p1, p2, [](PlayerObject* p) { return p->m_isShip; }))
            {
                clearLane(lane);
                L.heldApplied = down;
                return true;
            }

            if (L.delayCounter >= 0)
            {
                L.queuedOpposite = (down != L.queuedDown);
                L.framesSinceInput = 0;
                return false;
            }

            if (L.delayCounter == kFlushOppState)
            {
                clearLane(lane);
                const bool opp = !L.queuedDown;
                if (L.heldApplied != opp)
                {
                    sendButton(pl, lane, opp);
                }
            }

            const bool wave = anyMatches(ctl, p1, p2,
                [](PlayerObject* p) { return p->m_isDart; });
            const bool miniWave = anyMatches(ctl, p1, p2,
                [](PlayerObject* p) { return p->m_isDart && p->m_vehicleSize < 1.0f; });

            L.spamStreak = (L.framesSinceInput < kSpamGapFrames) ? L.spamStreak + 1 : 0;
            L.framesSinceInput = 0;

            if (wave && L.spamStreak >= kSpamStreakCap)
            {
                L.heldApplied = down;
                return true;
            }

            const bool releaseMatters = anyMatches(ctl, p1, p2,
                [](PlayerObject* p) { return p->m_isDart || p->m_isRobot; });
            if (!down && !releaseMatters)
            {
                L.heldApplied = false;
                return true;
            }

            L.queuedOpposite = false;
            const char* verb = down ? "PRESS" : "RELEASE";
            const char* who = (ctl.drivesP1 && ctl.drivesP2) ? "P1+P2" : (ctl.drivesP1 ? "P1" : "P2");

            const int range = miniWave ? kRangeMiniWave : (wave ? kRangeWave : kRangeOther);
            const int maxDelay = wave ? m_waveDelayLimit : m_cubeDelayLimit;

            const int base = simulateAhead(pl, p1, p2, ctl, 0, down, range);
            if (base >= range)
            {
                L.heldApplied = down;
                return true;
            }

            int pickedDelay = 0;
            int pickedFrames = base;

            auto choose = [&](int d, int t) { pickedDelay = d; pickedFrames = t; };

            for (int d = 1; d <= maxDelay; ++d)
            {
                const int t = simulateAhead(pl, p1, p2, ctl, d, down, range);

                if (wave)
                {
                    const int needed = d * 2 + 10;
                    if (t >= range && (t - base) >= d * 2 + 6)
                    {
                        choose(d, t);
                        break;
                    }
                    if (t >= base + needed && t > pickedFrames + 2)
                    {
                        choose(d, t);
                        if (t >= range) break;
                    }
                }
                else
                {
                    if (t >= range)
                    {
                        choose(d, t);
                        break;
                    }
                    if (t > pickedFrames + 12)
                    {
                        choose(d, t);
                    }
                }
            }

            if (pickedDelay > 0)
            {
                log::info("[BufferInputs] {} {} delayed {} ticks ({:.2f} frames @ 240 TPS) | base {} -> {} ticks survived [cap: {}]",
                    who, verb, pickedDelay, pickedDelay / 4.0f, base, pickedFrames, maxDelay);
                L.queuedDown = down;
                L.delayCounter = pickedDelay;
                return false;
            }

            L.heldApplied = down;
            return true;
        }

        void advanceLane(PlayLayer* pl, int lane)
        {
            InputLane& L = m_lanes[lane];

            if (L.framesSinceInput < kNoInputMark) ++L.framesSinceInput;

            if (laneIsShip(pl, lane))
            {
                clearLane(lane);
                syncHeld(pl, lane);
                return;
            }

            if (L.delayCounter > 0)
            {
                --L.delayCounter;
                return;
            }

            if (L.delayCounter == 0)
            {
                sendButton(pl, lane, L.queuedDown);
                const bool follow = L.queuedOpposite;
                L.queuedOpposite = false;
                L.delayCounter = follow ? kFlushOppState : kIdleState;
                return;
            }

            if (L.delayCounter == kFlushOppState)
            {
                sendButton(pl, lane, !L.queuedDown);
                L.queuedOpposite = false;
                L.delayCounter = kIdleState;
                return;
            }

            syncHeld(pl, lane);
        }

        void checkHotkey()
        {
#ifdef GEODE_IS_WINDOWS
            const bool pressed = GetActiveWindow() != nullptr &&
                (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
            if (pressed && !m_hotkeyHeld)
            {
                cycleMode();
            }
            m_hotkeyHeld = pressed;
#endif
        }

    public:
        void start(PlayLayer* pl)
        {
            stop();

            if (!pl) return;

            m_shadowP1 = makeShadow(pl);
            m_shadowP2 = makeShadow(pl);
            m_simRunning = false;
            m_isInjecting = false;
            resetLanes();
        }

        void stop()
        {
            destroyShadow(m_shadowP1);
            destroyShadow(m_shadowP2);
            m_simRunning = false;
            m_isInjecting = false;
            m_stateLog.clear();
            m_seenSet.clear();
            resetLanes();
        }

        void handleReset()
        {
            resetLanes();
        }

        void cycleMode()
        {
            m_modeIdx = (m_modeIdx + 1) % kModeCount;
            clearAllLanes();

            const ModeConfig& p = kModeTable[m_modeIdx];
            m_enabled = p.enabled;
            m_cubeDelayLimit = p.cubeDelay;
            m_waveDelayLimit = p.waveDelay;
            log::info("{}", p.label);

            if (m_notif)
            {
                m_notif->cancel();
                m_notif = nullptr;
            }

            if (Mod::get()->getSettingValue<bool>("show-notifcation"))
            {
                m_notif = Notification::create(p.label, NotificationIcon::None, 0.75f);
                m_notif->show();
            }
        }

        bool isSimulating() const { return m_simRunning; }
        bool isInjectingNow() const { return m_isInjecting; }

        void markBoundaryHit() { m_hitBoundary = true; }

        void trackObject(GameObject* o)
        {
            if (!o || !m_seenSet.insert(o).second) return;

            ObjectState s;
            s.target = o;
            s.wasActivated = o->m_isActivated;
            s.wasRingPowered = o->m_isRingPoweredOn;
            if (auto* e = typeinfo_cast<EnhancedGameObject*>(o))
            {
                s.isEnhanced = true;
                s.wasTriggered = e->m_activated;
                s.triggeredByP1 = e->m_activatedByPlayer1;
                s.triggeredByP2 = e->m_activatedByPlayer2;
                s.wasAnimTriggered = e->m_animationTriggered;
            }
            m_stateLog.push_back(s);
        }

        bool handleDeath(PlayerObject* who)
        {
            if (m_simRunning && who && (who == m_shadowP1 || who == m_shadowP2))
            {
                m_shadowDied = true;
                return true;
            }
            resetLanes();
            return false;
        }

        void updateDt(float dt)
        {
            PlayLayer* pl = PlayLayer::get();
            const float warp = pl ? pl->m_gameState.m_timeWarp : 0.f;
            m_cachedDt = (pl && warp > 0.f) ? dt / warp : dt;
        }

        bool processInput(GJBaseGameLayer* layer, bool down, bool isP1)
        {
            PlayLayer* pl = PlayLayer::get();
            if (!pl) return true;

            const bool dual = isDualMode(pl);
            const bool split = hasSplitControls(pl);
            const bool merged = dual && !split;
            const bool independent = dual && split;

            if (!isP1)
            {
                if (independent)
                {
                    return processLane(layer, pl, 1, down, dual);
                }
                return !(merged && m_enabled);
            }

            const bool pass = processLane(layer, pl, 0, down, dual);

            if (pass && merged && m_enabled)
            {
                m_isInjecting = true;
                pl->handleButton(down, 1, false);
                m_isInjecting = false;
            }
            return pass;
        }

        void onTick(PlayerObject* who)
        {
            PlayLayer* pl = PlayLayer::get();
            if (!pl || who != pl->m_player1) return;

            checkHotkey();

            if (!m_enabled) return;

            advanceLane(pl, 0);

            if (isSplitDual(pl))
            {
                advanceLane(pl, 1);
            }
            else if (m_lanes[1].delayCounter != kIdleState)
            {
                clearLane(1);
            }
        }
    };

    static Predictor g_predictor;

}

$on_mod(Loaded)
{
    log::info("[InputBuffer] v1.2.0 loaded!");
}

class $modify(BufPlayLayer, PlayLayer)
{
    static void onModify(auto& self)
    {
        ibuf::hookFirst(self, {
            "PlayLayer::destroyPlayer",
            "PlayLayer::playEndAnimationToPos",
            "PlayLayer::playGravityEffect",
            });
    }

    bool init(GJGameLevel * lvl, bool replay, bool noObjects)
    {
        if (!PlayLayer::init(lvl, replay, noObjects)) return false;
        ibuf::g_predictor.start(this);
        return true;
    }

    void resetLevel()
    {
        PlayLayer::resetLevel();
        ibuf::g_predictor.handleReset();
    }

    void destroyPlayer(PlayerObject * who, GameObject * cause)
    {
        if (ibuf::g_predictor.handleDeath(who)) return;
        PlayLayer::destroyPlayer(who, cause);
    }

    void playEndAnimationToPos(cocos2d::CCPoint pos)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(pos);
    }

    void playGravityEffect(bool flip)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayLayer::playGravityEffect(flip);
    }

    void onQuit()
    {
        PlayLayer::onQuit();
        ibuf::g_predictor.stop();
    }
};

class $modify(BufHardStreak, HardStreak)
{
    static void onModify(auto& self)
    {
        ibuf::hookFirst(self, { "HardStreak::addPoint" });
    }

    void addPoint(cocos2d::CCPoint pt)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        HardStreak::addPoint(pt);
    }
};

class $modify(BufEffectObject, EffectGameObject)
{
    static void onModify(auto& self)
    {
        ibuf::hookFirst(self, { "EffectGameObject::triggerObject" });
    }

    void triggerObject(GJBaseGameLayer * layer, int id, gd::vector<int> const* ids)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        EffectGameObject::triggerObject(layer, id, ids);
    }
};

class $modify(BufGameObject, GameObject)
{
    static void onModify(auto& self)
    {
        ibuf::hookFirst(self, { "GameObject::playShineEffect" });
    }

    void playShineEffect()
    {
        if (ibuf::g_predictor.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufRingObject, RingObject)
{
    static void onModify(auto& self)
    {
        ibuf::hookFirst(self, { "RingObject::spawnCircle" });
    }

    void spawnCircle()
    {
        if (ibuf::g_predictor.isSimulating()) return;
        RingObject::spawnCircle();
    }
};

class $modify(BufPlayerObject, PlayerObject)
{
    static void onModify(auto& self)
    {
        ibuf::hookFirst(self, {
            "PlayerObject::playSpiderDashEffect",
            "PlayerObject::incrementJumps",
            "PlayerObject::ringJump",
            "PlayerObject::spawnCircle",
            "PlayerObject::spawnDualCircle",
            "PlayerObject::spawnPortalCircle",
            "PlayerObject::spawnScaleCircle",
            "PlayerObject::playBumpEffect",
            });
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps()
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayerObject::incrementJumps();
    }

    void spawnCircle()
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayerObject::spawnCircle();
    }

    void spawnDualCircle()
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayerObject::spawnDualCircle();
    }

    void spawnPortalCircle(cocos2d::ccColor3B color, float startRadius)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void spawnScaleCircle()
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayerObject::spawnScaleCircle();
    }

    void playBumpEffect(int objectType, GameObject* player)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        PlayerObject::playBumpEffect(objectType, player);
    }

    void update(float dt)
    {
        const bool real = !ibuf::g_predictor.isSimulating();
        if (real)
        {
            ibuf::g_predictor.onTick(this);
        }
        PlayerObject::update(dt);
        if (real && PlayLayer::get())
        {
            ibuf::g_predictor.updateDt(dt);
        }
    }

    void ringJump(RingObject* ring, bool flag)
    {
        if (ibuf::g_predictor.isSimulating() && !ibuf::kAllowInteractables) return;
        PlayerObject::ringJump(ring, flag);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer)
{
    static void onModify(auto& self)
    {
        ibuf::hookFirst(self, {
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

    bool canBeActivatedByPlayer(PlayerObject * who, EffectGameObject * obj)
    {
        if (ibuf::g_predictor.isSimulating() && !ibuf::kAllowInteractables) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(who, obj);
    }

    void handleButton(bool down, int button, bool isP1)
    {
        const bool intercept = !ibuf::g_predictor.isSimulating() &&
            !ibuf::g_predictor.isInjectingNow() &&
            button == 1 &&
            PlayLayer::get();
        if (intercept && !ibuf::g_predictor.processInput(this, down, isP1))
        {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isP1);
    }

    void collisionCheckObjects(PlayerObject * who, gd::vector<GameObject*>*list, int count, float dt)
    {
        if (!ibuf::g_predictor.isSimulating() || !list)
        {
            GJBaseGameLayer::collisionCheckObjects(who, list, count, dt);
            return;
        }

        static gd::vector<GameObject*> kept;
        kept.clear();
        for (int i = 0; i < count; ++i)
        {
            GameObject* o = (*list)[i];
            if (!o) continue;

            const GameObjectType t = o->m_objectType;
            if (ibuf::isBlocking(t))
            {
                kept.push_back(o);
            }
            else if (ibuf::isTrackable(t))
            {
                ibuf::g_predictor.trackObject(o);
                kept.push_back(o);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(who, &kept, static_cast<int>(kept.size()), dt);
    }

    void playerTouchedRing(PlayerObject * who, RingObject * ring)
    {
        if (ibuf::g_predictor.isSimulating() && !ibuf::kAllowInteractables) return;
        GJBaseGameLayer::playerTouchedRing(who, ring);
    }

    void flipGravity(PlayerObject * who, bool flip, bool noEffects)
    {
        if (ibuf::g_predictor.isSimulating())
        {
            GJBaseGameLayer::flipGravity(who, flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(who, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID)
    {
        if (ibuf::g_predictor.isSimulating()) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void playerWillSwitchMode(PlayerObject * who, GameObject * obj)
    {
        if (ibuf::g_predictor.isSimulating())
        {
            ibuf::g_predictor.markBoundaryHit();
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(who, obj);
    }

    void toggleDualMode(GameObject * obj, bool dual, PlayerObject * who, bool noEffects)
    {
        if (ibuf::g_predictor.isSimulating())
        {
            ibuf::g_predictor.markBoundaryHit();
            return;
        }
        GJBaseGameLayer::toggleDualMode(obj, dual, who, noEffects);
    }

    void toggleFlipped(bool flip, bool noEffects)
    {
        if (ibuf::g_predictor.isSimulating())
        {
            ibuf::g_predictor.markBoundaryHit();
            return;
        }
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void teleportPlayer(TeleportPortalObject * obj, PlayerObject * who)
    {
        if (ibuf::g_predictor.isSimulating())
        {
            ibuf::g_predictor.markBoundaryHit();
            return;
        }
        GJBaseGameLayer::teleportPlayer(obj, who);
    }
};