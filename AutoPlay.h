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

// 🎱 Professional Physics Validator
struct PhysicalValidator {
    static constexpr double BALL_DIAMETER = 2.0 * Physics::BALL_RADIUS;
    static constexpr double MIN_POCKET_DISTANCE = 40.0;  // Minimum distance to pocket
    static constexpr double MAX_POCKET_DISTANCE = 120.0; // Maximum distance to pocket
    
    // Validate shot produces actual pocketing
    static bool validatePhysicalShot(const Prediction& pred, double power) {
        if (!pred.guiData.balls[0].onTable) return false;
        
        int pottedBalls = 0;
        for (int i = 1; i < pred.guiData.ballsCount; i++) {
            if (pred.guiData.balls[i].originalOnTable && !pred.guiData.balls[i].onTable) {
                pottedBalls++;
            }
        }
        
        return pottedBalls > 0;
    }

    // Calculate exact power needed for pocket distance
    static double calculateOptimalPower(
        const Point2D& cueBallPos,
        const Point2D& targetBallPos,
        const Point2D& pocketPos,
        const FrictionProperties& friction
    ) {
        Point2D toTarget = targetBallPos - cueBallPos;
        double cueToTargetDist = std::sqrt(toTarget.square());
        if (cueToTargetDist < 0.1) return 0.0;
        
        Point2D toPocket = pocketPos - targetBallPos;
        double targetToPocketDist = std::sqrt(toPocket.square());
        
        // Physics: velocity needed to reach pocket with friction
        double frictionCoeff = friction._velocityReductionRollingFactor * 9.81;
        double requiredVelocity = std::sqrt(frictionCoeff * targetToPocketDist * 2.0);
        
        // Normalize to power scale (0-666)
        double power = std::min(requiredVelocity / 1.0, 666.0);
        return power > 0.0 ? power : 100.0;
    }

    // Validate pocket is reachable from target ball
    static bool isPocketReachable(
        const Point2D& targetBallPos,
        const Point2D& pocketPos
    ) {
        Point2D delta = pocketPos - targetBallPos;
        double distance = std::sqrt(delta.square());
        return distance >= MIN_POCKET_DISTANCE && distance <= MAX_POCKET_DISTANCE;
    }

    // Calculate collision point accuracy (0.0 to 1.0)
    static double getCollisionAccuracy(
        const Point2D& cueBallPos,
        const Point2D& targetBallPos,
        const Point2D& pocketPos
    ) {
        Point2D toTarget = targetBallPos - cueBallPos;
        Point2D toPocket = pocketPos - targetBallPos;
        
        double toTargetLen = std::sqrt(toTarget.square());
        double toPocketLen = std::sqrt(toPocket.square());
        
        if (toTargetLen < 0.1 || toPocketLen < 0.1) return 0.0;
        
        Point2D targetDir = toTarget * (1.0 / toTargetLen);
        Point2D pocketDir = toPocket * (1.0 / toPocketLen);
        
        double dot = targetDir.x * pocketDir.x + targetDir.y * pocketDir.y;
        return std::max(0.0, std::min(1.0, dot));
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
    
    enum ScanMode { FAST, SLOW } scan = FAST;

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
    
    // Professional ScanFast - Accurate trajectory & power calculation
    void ScanFast(double angleStep = 0.05f) {
        if (g_CurrentCandidate.idx != -1) return;
        if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;

        Ball::Classification myclass = sharedGameManager.getPlayerClassification();
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        
        std::vector<Candidate> candidates;
        auto pockets = getPockets();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        bool bFoundLowestNumberedBall = false;
        int iFoundLowestNumberedBall = -1;
        bool isNineBallGame = myclass == Ball::Classification::NINE_BALL_RULE;
        
        // Iterate through all balls on table
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            if (isNineBallGame && bFoundLowestNumberedBall) break;

            auto& ball = gPrediction->guiData.balls[i];
            if (!ball.originalOnTable) continue;

            if (!bFoundLowestNumberedBall) {
                bFoundLowestNumberedBall = true;
                iFoundLowestNumberedBall = i;
            }

            // Check if this ball is a valid candidate
            bool isCandidate = false;
            if (isNineBallGame) {
                isCandidate = (i == iFoundLowestNumberedBall);
            } else if (myclass == Ball::Classification::ANY) {
                isCandidate = (ball.classification != Ball::Classification::EIGHT_BALL &&
                              ball.classification != Ball::Classification::CUE_BALL);
            } else {
                // 8-ball: prioritize YOUR balls
                isCandidate = (ball.classification == myclass || 
                              (ball.classification != Ball::Classification::EIGHT_BALL &&
                               ball.classification != Ball::Classification::CUE_BALL));
            }
            
            if (!isCandidate) continue;

            // Try all pockets for this ball
            for (int pocketIdx = 0; pocketIdx < pockets.size(); pocketIdx++) {
                if (nominatedPocket < 6 && pocketIdx != nominatedPocket) continue;

                Point2D pocket = pockets[pocketIdx];
                Point2D toPocket = pocket - ball.initialPosition;
                double pocketDist = std::sqrt(toPocket.square());
                
                // Skip if pocket too close/far
                if (pocketDist < PhysicalValidator::MIN_POCKET_DISTANCE) continue;
                if (pocketDist > PhysicalValidator::MAX_POCKET_DISTANCE) continue;
                
                // Calculate ghost ball for collision
                Point2D pocketDir = toPocket * (1.0 / pocketDist);
                Point2D ghostBallPos = ball.initialPosition - pocketDir * PhysicalValidator::BALL_DIAMETER;
                
                Point2D shotLine = ghostBallPos - cueBall.initialPosition;
                double shotDist = std::sqrt(shotLine.square());
                if (shotDist < 0.1) continue;
                
                double angle = std::atan2(shotLine.y, shotLine.x);
                if (angle < 0) angle += 2 * M_PI;
                
                // Calculate collision accuracy (0.0-1.0)
                double accuracy = PhysicalValidator::getCollisionAccuracy(
                    cueBall.initialPosition,
                    ball.initialPosition,
                    pocket
                );
                
                // Calculate optimal power for this pocket distance
                double power = PhysicalValidator::calculateOptimalPower(
                    cueBall.initialPosition,
                    ball.initialPosition,
                    pocket,
                    cachedFriction
                );
                
                // Score: lower = better. Prioritize accuracy and YOUR balls
                double score = pocketDist + shotDist;
                score *= (1.0 - accuracy);  // Reduce score for high accuracy
                
                // YOUR balls get priority
                if (ball.classification == myclass && myclass != Ball::Classification::ANY) {
                    score *= 0.3;  // Strong priority
                } else if (ball.classification != myclass && myclass != Ball::Classification::ANY) {
                    score *= 2.0;  // Lower priority for opponent balls
                }
                
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
            if (gPrediction->guiData.balls[cand.idx].onTable) continue;
            if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) continue;

            if (myclass == Ball::Classification::NINE_BALL_RULE) {
                auto firstHit = gPrediction->guiData.collision.firstHitBall;
                if (!firstHit || firstHit->index != cand.idx) continue;

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
                LOGI("AutoPlay: 9ball Found angle %f power %f", angle, cand.power);
                g_CurrentCandidate = cand;
                g_CurrentCandidate.idx = bestPottedIdx;
                g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[bestPottedIdx].pocketIndex;
                foundShot = true;
                Shoot(angle, cand.power);
                break;
            }

            // Validate ball pocketing
            bool isValidShot = false;
            auto& targetBall = gPrediction->guiData.balls[cand.idx];
            
            if (myclass == Ball::Classification::ANY) {
                isValidShot = (targetBall.classification != Ball::Classification::CUE_BALL && 
                              targetBall.classification != Ball::Classification::EIGHT_BALL);
            } else {
                isValidShot = (targetBall.classification == myclass);
            }

            if (!isValidShot) continue;

            // Final validation
            auto& cueBallRef = gPrediction->guiData.balls[0];
            if (cueBallRef.originalOnTable && !cueBallRef.onTable) continue;
            
            auto& eightBallRef = gPrediction->guiData.balls[8];
            if (eightBallRef.originalOnTable && !eightBallRef.onTable && myclass != Ball::Classification::EIGHT_BALL) continue;
            
            if (PhysicalValidator::validatePhysicalShot(*gPrediction, cand.power)) {
                LOGI("AutoPlay: Found angle %f power %f", angle, cand.power);
                g_CurrentCandidate = cand;
                foundShot = true;
                Shoot(angle, cand.power);
                break;
            }
        }

        if (!foundShot) {
            lastFailedCuePos = cueBall.initialPosition;
            LOGI("AutoPlay: ScanFast failed, trying ScanSlow");
            scan = SLOW;
        }
    }

    // Professional ScanSlow - Exhaustive but efficient
    void ScanSlow(double angleStep = 0.02f) {
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
        int stepsPerFrame = (int)(15 * GameSpeed::getAnimationMultiplier());
        
        while (steps < stepsPerFrame && currentScanAngle < maxAngle) {
            double angle = currentScanAngle;
            currentScanAngle += angleStep;
            steps++;

            // Test power range
            std::vector<double> powers = {666.0, 500.0, 333.0, 200.0, 100.0};
            
            for (double power : powers) {
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
                
                int targetIdx = -1;

                // Find potted ball
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (!ball.originalOnTable || ball.onTable) continue;

                    bool isValid = false;
                    if (myclass == Ball::Classification::ANY) {
                        isValid = (ball.classification != Ball::Classification::CUE_BALL && 
                                  ball.classification != Ball::Classification::EIGHT_BALL);
                    } else if (myclass == Ball::Classification::NINE_BALL_RULE) {
                        // For 9-ball, check lowest ball is hit first
                        for (int j = 1; j < gPrediction->guiData.ballsCount; j++) {
                            if (gPrediction->guiData.balls[j].originalOnTable) {
                                auto firstHit = gPrediction->guiData.collision.firstHitBall;
                                if (firstHit && firstHit->index == j && i == j) {
                                    isValid = true;
                                }
                                break;
                            }
                        }
                    } else {
                        isValid = (ball.classification == myclass);
                    }
                    
                    if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) isValid = false;
                    if (isValid) { targetIdx = i; break; }
                }

                if (targetIdx == -1) continue;
                if (!gPrediction->guiData.balls[0].onTable) continue;
                if (!gPrediction->guiData.balls[8].onTable && myclass != Ball::Classification::EIGHT_BALL) continue;

                auto firstHit = gPrediction->guiData.collision.firstHitBall;
                if (!firstHit) continue;
                
                if (myclass == Ball::Classification::ANY && firstHit->classification == Ball::Classification::EIGHT_BALL) continue;
                if (myclass != Ball::Classification::ANY && firstHit->classification != myclass) continue;

                if (PhysicalValidator::validatePhysicalShot(*gPrediction, power)) {
                    LOGI("AutoPlaySlow: Found angle %f power %f", angle, power);
                    g_CurrentCandidate.idx = targetIdx;
                    g_CurrentCandidate.angle = angle;
                    g_CurrentCandidate.power = power;
                    g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;
                    Shoot(angle, power);
                    return;
                }
            }
        }

        if (currentScanAngle >= maxAngle) {
            LOGI("AutoPlaySlow: Complete scan finished");
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
        return activeAction != nullptr;
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
            else {
                DrawEightBallLoading(GetForegroundDrawList());
                ScanSlow(0.02f);
            }
        } 
        if (state == NOMINATING) {
            nominationFrameCounter++;
            if (nominationFrameCounter == 10) {
                buttonClicker.Click(GetPocketScreenPos(g_CurrentCandidate.pocketIndex));
            }
            if (nominationFrameCounter > 20 && !buttonClicker.Active) {
                takeShot(g_CurrentCandidate.angle, g_CurrentCandidate.power);
                ClearState();
                state = IDLE;
            }
        }
    }
};
