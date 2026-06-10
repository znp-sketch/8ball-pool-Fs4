#pragma once

#include "Prediction.fast.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>

#include "ScreenTable.h"
#include "PhysicsModel.h"
#include "GameSpeedControl.h"
#include "FrictionProperties.h"
#include "ButtonClicker.h"

using namespace ImGui;

Candidate g_CurrentCandidate = { -1 };

extern void DrawEightBallLoading(ImDrawList*);

ImVec2 GetPocketScreenPos(int pocketIdx) {
    Table table = sharedGameManager.mTable;
    if (!table) return {};

    auto tableProperties = table.mTableProperties();
    if (!tableProperties) return {};

    auto& pockets = tableProperties.mPockets();
    return WorldToScreen(pockets[pocketIdx]);
}

// 🎱 Physics-based validation system
struct PhysicalValidator {
    // Validate shot using realistic physics
    static bool validatePhysicalShot(const Prediction& pred, double power) {
        // Check cue ball survival
        if (!pred.guiData.balls[0].onTable) return false;
        
        // Check target ball delivery
        int pottedBalls = 0;
        for (int i = 1; i < pred.guiData.ballsCount; i++) {
            if (pred.guiData.balls[i].originalOnTable && !pred.guiData.balls[i].onTable) {
                pottedBalls++;
            }
        }
        
        // Ensure at least one ball was potted
        return pottedBalls > 0;
    }

    // Calculate realistic power based on distance and friction
    static double calculateRealisticPower(
        const Point2D& cueBallPos,
        const Point2D& targetBallPos,
        const Point2D& pocketPos,
        const FrictionProperties& friction
    ) {
        Point2D toTarget = targetBallPos - cueBallPos;
        double distToCueBall = std::sqrt(toTarget.square());
        
        Point2D toPocket = pocketPos - targetBallPos;
        double distToPocket = std::sqrt(toPocket.square());
        
        // Total distance ball must travel
        double totalDistance = distToCueBall + distToPocket;
        
        // Physics: v² = 2 * a * s, where a = friction deceleration
        double frictionDeceleration = 9.81 * friction._velocityReductionRollingFactor;
        double requiredVelocity = std::sqrt(2.0 * frictionDeceleration * totalDistance);
        
        // Cap at max power
        return std::min(requiredVelocity, 666.0);
    }

    // Factor in spin effects on power requirements
    static double adjustPowerForSpin(double basePower, const Vec2d& shotSpin, double spinFactor = 0.15) {
        double spinMagnitude = std::sqrt(shotSpin.x * shotSpin.x + shotSpin.y * shotSpin.y);
        double adjustment = 1.0 + (spinMagnitude * spinFactor);
        return std::min(basePower * adjustment, 666.0);
    }

    // Validate pocket accessibility
    static bool isPocketAccessible(
        const Point2D& targetBallPos,
        const Point2D& pocketPos,
        double ballRadius = Physics::BALL_RADIUS
    ) {
        Point2D delta = pocketPos - targetBallPos;
        double distance = std::sqrt(delta.square());
        
        // Ball must be close enough to pocket to enter
        constexpr double POCKET_MARGIN = 2.0;
        return distance < POCKET_MARGIN && distance > ballRadius * 0.5;
    }
};

bool IsShotValid() {
    auto& cand = g_CurrentCandidate;
    if (cand.idx == -1) return false;

    Ball::Classification myclass = sharedGameManager.getPlayerClassification();

    uint nominatedPocket = sharedGameManager.getNominatedPocket();
    if (nominatedPocket < 6 && cand.pocketIndex != nominatedPocket) return false;

    if (!gPrediction->guiData.balls[0].onTable) return false;
    if (!gPrediction->guiData.balls[cand.idx].originalOnTable) return false;
    if (gPrediction->guiData.balls[cand.idx].onTable) return false;
    if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) return false;

    auto& ball8 = gPrediction->guiData.balls[8];
    if (myclass == Ball::Classification::ANY && ball8.originalOnTable && !ball8.onTable) return false;

    auto& firstHit = gPrediction->guiData.collision.firstHitBall;
    if (firstHit) {
        if (myclass == Ball::Classification::ANY) {
            if (firstHit->classification == Ball::Classification::EIGHT_BALL) return false;
        } else if (firstHit->classification != myclass) return false;
    }

    return true;
}

Point2D lastFailedCuePos = { -1000.0, -1000.0 };

namespace AutoPlay {
    double lastSetAngle = 0.f;
    double lastSetPower = 0.f;
    bool didSetAngle = false;
    bool bAutoPlaying = false;
    
    static float scanDelayTimer = 0.0f;
    static float shotExecutionTimer = 0.0f;
    static FrictionProperties cachedFriction = {0.2, 0.0111, 0.025, 0.0014577259475218659, 196, 10.878, 9.8};

    enum State {
        IDLE,
        SCANNING,
        NOMINATING,
        EXECUTING,
    } state = IDLE;
    
    double pendingShotPower = 0.f;
    double pendingShotAngle = 0.f;
    int nominationFrameCounter = 0;
    
    enum ScanMode {
        FAST,
        SLOW,
    } scan = FAST;

    bool shouldAutoPlay() { 
        return !didSetAngle || lastSetAngle == sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(); 
    }

    void setAimAngle(double angle) {
        lastSetAngle = angle;
        sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(angle);
    }

    void setShotPower(double power) {
        lastSetPower = power;
        sharedGameManager.mVisualCue().setShotPower(power);
    }

    void takeShot(double angle, double power) {
        setAimAngle(angle);
        setShotPower(power);
        gPrediction->determineShotResult(false, angle, power);

        sharedGameManager.mVisualCue().mPower(ShotPowerToPower(power));
        M(void, libmain + 0x2dc0c58, void*)(F(void*, sharedGameManager + 0x3b0));
    }
    
    void ClearState() {
        g_CurrentCandidate.idx = -1;
        lastFailedCuePos = { -1000.0, -1000.0 };
        scanDelayTimer = 0.0f;
        shotExecutionTimer = 0.0f;
    }
    
    void Shoot(double angle, double power = 0.f) {
        setAimAngle(angle);
        setShotPower(power);
        gPrediction->determineShotResult(false, angle, power);

        bool nominating = false;
        int nominationMode = sharedGameManager.getPocketNominationMode();
        auto myclass = sharedGameManager.getPlayerClassification();
        if ((nominationMode == 1 && myclass == Ball::Classification::EIGHT_BALL) || 
            (nominationMode == 2 && myclass != Ball::Classification::ANY)) {
            if (g_CurrentCandidate.idx != -1 && sharedGameManager.getNominatedPocket() != g_CurrentCandidate.pocketIndex) {
                nominating = true;
            }
        }

        if (nominating) {
            pendingShotPower = power;
            pendingShotAngle = angle;
            state = NOMINATING;
            nominationFrameCounter = 0;
        } else {
            takeShot(angle, power);
            ClearState();
            state = IDLE;
        }
    }
    
    // Enhanced ScanFast with physics-based power calculation - ATTEMPTS ALL BALLS
    void ScanFast(double angleStep = 0.1f) {
        if (g_CurrentCandidate.idx != -1) return;
        if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;

        double startingAngle = sharedGameManager.mVisualCue().mVisualGuide().mAimAngle();
        
        gPrediction->determineShotResult(true, startingAngle);
        std::vector<int> startingPottedBalls;
        for (int i = 0; i < gPrediction->guiData.ballsCount; i++) {
            Prediction::Ball& ball = gPrediction->guiData.balls[i];
            if (ball.originalOnTable && !ball.onTable) {
                startingPottedBalls.push_back(i);
            }
        }
        
        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        
        std::vector<Candidate> candidates;
        
        auto pockets = getPockets();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        bool bFoundLowestNumberedBall = false;
        int iFoundLowestNumberedBall = -1;
        bool isNineBallGame = myclass == Ball::Classification::NINE_BALL_RULE;
        
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            if (isNineBallGame && bFoundLowestNumberedBall) break;

            auto& ball = gPrediction->guiData.balls[i];
            if (!ball.originalOnTable) continue;

            if (!bFoundLowestNumberedBall) {
                bFoundLowestNumberedBall = true;
                iFoundLowestNumberedBall = i;
            }

            // MODIFIED: Attempt ALL valid balls, not just matching classification
            bool isACandidate = false;
            
            if (isNineBallGame) {
                // In 9-ball, only hit the lowest numbered ball
                isACandidate = (i == iFoundLowestNumberedBall);
            } else if (myclass == Ball::Classification::ANY) {
                // Solids/Stripes: hit any ball except 8-ball
                isACandidate = (ball.classification != Ball::Classification::EIGHT_BALL &&
                               ball.classification != Ball::Classification::CUE_BALL);
            } else {
                // Regular 8-ball: attempt ALL balls
                isACandidate = true;
            }
            
            if (!isACandidate) continue;

            for (int pocketIdx = 0; pocketIdx < pockets.size(); pocketIdx++) {
                if (nominatedPocket < 6 && pocketIdx != nominatedPocket) continue;

                Point2D pocket = pockets[pocketIdx];
                Point2D toPocket = pocket - ball.initialPosition;
                double distTargetToPocket = sqrt(toPocket.square());
                if (distTargetToPocket < 0.1) continue;
                
                Point2D direction = toPocket * (1.0 / distTargetToPocket);
                Point2D ghostBallPos = ball.initialPosition - direction * (2.0 * Physics::BALL_RADIUS);
                Point2D shotLine = ghostBallPos - cueBall.initialPosition;
                double distCueToTarget = sqrt(shotLine.square());
                double angle = atan2(shotLine.y, shotLine.x);
                
                if (angle < 0) angle += 2 * M_PI;
                
                double score = distCueToTarget + distTargetToPocket;
                
                // Prioritize easy balls (YOUR balls) over difficult ones
                if (myclass != Ball::Classification::ANY && ball.classification == myclass) {
                    score *= 0.5;  // Easy balls get priority (lower score)
                } else if (myclass != Ball::Classification::ANY && ball.classification != Ball::Classification::EIGHT_BALL) {
                    score *= 2.0;  // Opponent balls are backup (higher score)
                }
                
                // 🎱 Physics-based power calculation
                double basePower = PhysicalValidator::calculateRealisticPower(
                    cueBall.initialPosition,
                    ball.initialPosition,
                    pocket,
                    cachedFriction
                );
                
                // Adjust for spin
                auto spin = sharedGameManager.getShotSpin();
                double power = PhysicalValidator::adjustPowerForSpin(basePower, spin);
                
                if (power > 666.0) power = 666.0;
                
                candidates.push_back({i, angle, score, pocketIdx, power});
            }
        }
        
        std::sort(candidates.begin(), candidates.end());
        
        bool foundShot = false;
        for (const auto& cand : candidates) {
            double angle = NumberUtils::normalizeDoublePrecision(normalizeAngle(cand.angle));
            gPrediction->determineShotResult(true, angle, cand.power, sharedGameManager.getShotSpin(), cand);
            
            if (!gPrediction->firstHitIsTarget) continue;
            if (!gPrediction->guiData.balls[0].onTable) continue;

            if (myclass == Ball::Classification::NINE_BALL_RULE) {
                auto firstHit = gPrediction->guiData.collision.firstHitBall;
                if (!firstHit) continue;
                if (firstHit->index != cand.idx) continue;

                int bestPottedIdx = -1;
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (ball.originalOnTable && !ball.onTable) {
                        if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) continue;
                        
                        if (i == 9) { bestPottedIdx = 9; break; }
                        if (bestPottedIdx == -1 || i == cand.idx) bestPottedIdx = i;
                    }
                }

                if (bestPottedIdx == -1) continue;
                int effectiveTargetIdx = bestPottedIdx;

                if (nominatedPocket < 6 && gPrediction->guiData.balls[effectiveTargetIdx].pocketIndex != nominatedPocket) continue;

                LOGI("AutoPlay: 9ball: Found good angle %f with power %f", angle, cand.power);
                g_CurrentCandidate = cand;
                g_CurrentCandidate.idx = effectiveTargetIdx;
                g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[effectiveTargetIdx].pocketIndex;

                foundShot = true;
                Shoot(angle, cand.power);
                break;
            }

            if (gPrediction->guiData.balls[cand.idx].onTable) continue;
            if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) continue;

            std::vector<int> currentPottedBalls;
            bool isAngleGood = false;
            
            auto& targetBall = gPrediction->guiData.balls[cand.idx];
            
            // Check if correct ball was potted
            if (myclass == Ball::Classification::ANY) {
                // Solids/Stripes: any ball except 8-ball is good
                isAngleGood = (targetBall.classification != Ball::Classification::CUE_BALL && 
                              targetBall.classification != Ball::Classification::EIGHT_BALL);
            } else {
                // Regular 8-ball: YOUR ball should be potted
                isAngleGood = (targetBall.classification == myclass);
            }

            if (isAngleGood && gPrediction->guiData.collision.firstHitBall) {
                 auto firstHit = gPrediction->guiData.collision.firstHitBall;
                 if (myclass != Ball::Classification::ANY && firstHit->classification != myclass) isAngleGood = false;
                 else if (myclass == Ball::Classification::ANY && firstHit->classification == Ball::Classification::EIGHT_BALL) isAngleGood = false;
            }

            auto& cueBallRef = gPrediction->guiData.balls[0];
            if (isAngleGood && cueBallRef.originalOnTable && !cueBallRef.onTable) isAngleGood = false;
            
            auto& eightBallRef = gPrediction->guiData.balls[8];
            bool isEightBallPotted = eightBallRef.originalOnTable && !eightBallRef.onTable;
            if (isAngleGood && isEightBallPotted && myclass != Ball::Classification::EIGHT_BALL) isAngleGood = false;
            
            // 🎱 Apply physical validation
            if (isAngleGood && PhysicalValidator::validatePhysicalShot(*gPrediction, cand.power)) {
                LOGI("AutoPlay: Found good angle %f with physics-validated power %f", angle, cand.power);
                g_CurrentCandidate = cand;
                foundShot = true;
                Shoot(angle, cand.power);
                break;
            }
        }

        if (!foundShot) {
            lastFailedCuePos = cueBall.initialPosition;
            LOGI("AutoPlay: No good angle found after smart scan.");
            scan = SLOW;
        }
    }

    // Enhanced ScanSlow with improved physics modeling
    void ScanSlow(double angleStep = 0.01f) {
        static double currentScanAngle = 0.0;
        static bool isScanning = false;
        static Point2D lastScanCuePos = { -1000.0, -1000.0 };

        if (g_CurrentCandidate.idx != -1) return;
        
        if (!isScanning || gPrediction->guiData.balls[0].initialPosition != lastScanCuePos) {
            currentScanAngle = 0.0;
            isScanning = true;
            lastScanCuePos = gPrediction->guiData.balls[0].initialPosition;
        }

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        int steps = 0;
        bool foundShot = false;
        
        int stepsPerFrame = (int)(10 * GameSpeed::getAnimationMultiplier());
        
        while (steps < stepsPerFrame && currentScanAngle < maxAngle) {
            double angle = currentScanAngle;
            currentScanAngle += angleStep;
            steps++;

            // 🎱 Physics-based power progression
            std::vector<double> powers;
            for (double p = 666.0; p >= 100.0; p -= 100.0) {
                powers.push_back(p);
            }
            
            for (double power : powers) {
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
                
                bool isPotentiallyValid = false;
                int targetIdx = -1;

                bool bFoundLowestNumberedBall = false;
                int iFoundLowestNumberedBall = -1;
                bool isNineBallGame = myclass == Ball::Classification::NINE_BALL_RULE;

                if (isNineBallGame) {
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        auto& ball = gPrediction->guiData.balls[i];
                        if (!ball.originalOnTable) continue;

                        bFoundLowestNumberedBall = true;
                        iFoundLowestNumberedBall = i;
                        break;
                    }

                    auto firstHit = gPrediction->guiData.collision.firstHitBall;
                    if (!firstHit) continue;
                    
                    if (firstHit->index != iFoundLowestNumberedBall) continue;
                    if (!gPrediction->guiData.balls[0].onTable) continue;

                    int bestPottedIdx = -1;
                    for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                        auto& ball = gPrediction->guiData.balls[i];
                        if (ball.originalOnTable && !ball.onTable) {
                            if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) continue;
                            
                            if (i == 9) { bestPottedIdx = 9; break; }
                            if (bestPottedIdx == -1 || i == firstHit->index) bestPottedIdx = i;
                        }
                    }

                    if (bestPottedIdx == -1) continue;
                    targetIdx = bestPottedIdx;

                    LOGI("AutoPlay: 9ball: Found good angle %f with power %f", angle, power);
                    
                    g_CurrentCandidate.idx = targetIdx;
                    g_CurrentCandidate.angle = angle;
                    g_CurrentCandidate.power = power;
                    g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;

                    foundShot = true;
                    Shoot(angle, power);
                    break;
                }

                // MODIFIED: Check ALL balls for validity
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (ball.originalOnTable && !ball.onTable) {
                        bool isValidTarget = false;
                        
                        if (myclass == Ball::Classification::ANY) {
                            // Solids/Stripes: any ball except 8-ball
                            if (ball.classification != Ball::Classification::CUE_BALL && 
                                ball.classification != Ball::Classification::EIGHT_BALL) {
                                isValidTarget = true;
                            }
                        } else {
                            // Regular 8-ball: attempt ALL balls
                            if (ball.classification == myclass || 
                                (ball.classification != Ball::Classification::EIGHT_BALL &&
                                 ball.classification != Ball::Classification::CUE_BALL)) {
                                isValidTarget = true;
                            }
                        }
                        
                        if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) isValidTarget = false;

                        if (isValidTarget) {
                            targetIdx = i;
                            break;
                        }
                    }
                }

                if (targetIdx != -1) {
                    if (!gPrediction->guiData.balls[0].onTable) continue;
                    if (!gPrediction->guiData.balls[8].onTable && myclass != Ball::Classification::EIGHT_BALL) continue;

                    auto firstHit = gPrediction->guiData.collision.firstHitBall;
                    if (!firstHit) continue;
                    
                    if (myclass == Ball::Classification::ANY) {
                        if (firstHit->classification == Ball::Classification::EIGHT_BALL) continue;
                    } else if (firstHit->classification != myclass) continue;

                    // 🎱 Physical validation
                    if (PhysicalValidator::validatePhysicalShot(*gPrediction, power)) {
                        isPotentiallyValid = true;
                        g_CurrentCandidate.idx = targetIdx;
                        g_CurrentCandidate.angle = angle;
                        g_CurrentCandidate.power = power;
                        g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;
                    }
                }

                if (isPotentiallyValid) {
                    LOGI("AutoPlaySlow: Found physical shot at angle %f power %f", angle, power);
                    foundShot = true;
                    Shoot(angle, power);
                    break;
                }
            }

            if (foundShot) break;
        }

        if (!foundShot && currentScanAngle >= maxAngle) {
            LOGI("AutoPlaySlow: Finished scan, nothing found.");
            isScanning = false;
            currentScanAngle = 0.0;
            state = IDLE;
        }
    }

    void DrawToggleButton() {
        ImGuiIO& io = GetIO();
        float padding = 30.0f;
        int buttons = 1;
        float button_size = ImGui::GetFrameHeight() * 2.3f;
        float windowWidth = button_size * buttons + (buttons > 1 ? GetStyle().ItemSpacing.x * (buttons - 1) : 0) + GetStyle().WindowPadding.x * 2;
        float windowHeight = button_size + GetStyle().WindowPadding.y * 2;

        SetNextWindowPos(ImVec2(io.DisplaySize.x - 155 - windowWidth, io.DisplaySize.y - 20 - windowHeight), ImGuiCond_Always);
        SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
        
        PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
        PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
        
        if (Begin("AutoPlay", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
            auto DrawPlayPauseButton = [&](bool isPause) -> bool {
                ImVec2 pos = GetCursorScreenPos();
                ImVec2 size(button_size, button_size);
                ImVec2 end(pos.x + size.x, pos.y + size.y);
                ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
                
                PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 50, 180));
                PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 80, 200));
                PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(100, 100, 100, 200));
                PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
                
                bool clicked = Button("##AutoPlayBtn", size);
                
                ImDrawList* dl = GetWindowDrawList();
                float h = size.y * 0.4f;
                float w = h * 0.8f;

                if (isPause) {
                    float bar_w = w * 0.35f;
                    float gap = w * 0.3f;
                    dl->AddRectFilled(ImVec2(center.x - gap/2 - bar_w, center.y - h/2), ImVec2(center.x - gap/2, center.y + h/2), IM_COL32(255, 255, 255, 180));
                    dl->AddRectFilled(ImVec2(center.x + gap/2, center.y - h/2), ImVec2(center.x + gap/2 + bar_w, center.y + h/2), IM_COL32(255, 255, 255, 180));
                } else {
                    float off_x = h * 0.3f;
                    dl->AddTriangleFilled(ImVec2(center.x - off_x, center.y - h/2), ImVec2(center.x - off_x, center.y + h/2), ImVec2(center.x + off_x * 1.5f, center.y), IM_COL32(255, 255, 255, 180));
                }
                
                GetForegroundDrawList()->AddRect(pos, end, IM_COL32(200, 200, 200, 255), 5.0f, 0, 2.0f);
                
                PopStyleColor(4);
                return clicked;
            };

            if (DrawPlayPauseButton(bAutoPlaying)) {
                bAutoPlaying = !bAutoPlaying;
                if (bAutoPlaying) ClearState();
            }
        } 
        End();

        PopStyleVar();
        PopStyleColor(2);
    }

    bool isAnimationActive() {
        auto visualCue = sharedGameManager.mVisualCue();
        if (!visualCue) return true;
        
        auto _powerBarView = F(ptr, visualCue + 0x510);
        if (!_powerBarView) return true;

        auto activeAction = M(ptr, libmain + 0x2de6f30, ptr)(_powerBarView);
        if (activeAction) {
            return true;
        }

        return false;
    }
    
    void Update() {
        buttonClicker.Update();
        DrawToggleButton();

        if (isAnimationActive()) return;

        if (!bAutoPlaying || !sharedGameManager.mStateManager().isPlayerTurn()) {
            state = IDLE;
            return;
        }

        if (state == IDLE) {
            state = SCANNING;
            scan = FAST;
        } 
        if (state == SCANNING) {
            if (scan == FAST) ScanFast();
            if (scan == SLOW) {
                DrawEightBallLoading(GetForegroundDrawList());
                ScanSlow(0.003f);
            }
        } 
        if (state == NOMINATING) {
            nominationFrameCounter++;
            if (nominationFrameCounter == 10) {
                buttonClicker.Click(GetPocketScreenPos(g_CurrentCandidate.pocketIndex));
            }
            if (nominationFrameCounter > 20 && !buttonClicker.Active) {
                takeShot(pendingShotAngle, pendingShotPower);
                ClearState();
                state = IDLE;
            }
        }
    }
};
