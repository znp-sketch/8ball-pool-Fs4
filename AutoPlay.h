#pragma once

#include "Prediction.fast.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <vector>

#include "ScreenTable.h"
#include "PhysicsModel.h"
#include "GameSpeedControl.h"
#include "FrictionProperties.h"
#include "ButtonClicker.h"

using namespace ImGui;

// ============================================================================
// CONSTANTS & CONFIGURATION
// ============================================================================
const double PI = 3.14159265358979323846;
const double TWO_PI = 2.0 * PI;
const double ANGLE_STEP_FAST = 0.05;      // 0.05 radians (~2.86 degrees)
const double ANGLE_STEP_SLOW = 0.02;      // 0.02 radians (~1.15 degrees)
const double MIN_POCKET_DIST = 40.0;      // Minimum distance to pocket
const double MAX_POCKET_DIST = 120.0;     // Maximum distance to pocket
const double BALL_SAFETY_MARGIN = 5.0;    // Safety margin around ball

// Ball type classifications
enum BallType {
    CUE_BALL = 0,
    SOLIDS = 1,      // 1-7
    STRIPES = 2,     // 9-15
    EIGHT_BALL = 3,
    INVALID = -1
};

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

// ============================================================================
// PHYSICS ENGINE
// ============================================================================
struct PhysicsEngine {
    static constexpr double BALL_DIAMETER = 2.0 * Physics::BALL_RADIUS;
    static constexpr double GRAVITY = 9.81;
    
    // ========================================================================
    // EQUATION 1: Calculate power required for target distance
    // Physics: v^2 = 2*a*s
    // where: v = velocity, a = deceleration (friction), s = distance
    // ========================================================================
    static double calculatePowerForDistance(
        double targetDistance,
        const FrictionProperties& friction
    ) {
        if (targetDistance < 1.0) return 100.0;
        
        // Friction deceleration
        double friction_coeff = friction._velocityReductionRollingFactor;
        double deceleration = GRAVITY * friction_coeff;
        
        // v = sqrt(2 * a * s)
        double requiredVelocity = std::sqrt(2.0 * deceleration * targetDistance);
        
        // Map velocity to power (0-666 scale)
        double power = requiredVelocity / 1.0;  // Scaling factor
        return std::min(std::max(power, 100.0), 666.0);
    }
    
    // ========================================================================
    // EQUATION 2: Calculate trajectory angle to pocket
    // Physics: angle = atan2(dy, dx)
    // where: dy = y_pocket - y_ball, dx = x_pocket - x_ball
    // ========================================================================
    static double calculateTrajectoryAngle(
        const Point2D& fromPos,
        const Point2D& toPos
    ) {
        Point2D delta = toPos - fromPos;
        double angle = std::atan2(delta.y, delta.x);
        
        // Normalize to [0, 2π)
        if (angle < 0) angle += TWO_PI;
        
        return angle;
    }
    
    // ========================================================================
    // EQUATION 3: Calculate ghost ball position for collision
    // Physics: Ghost position = ball_center - (direction * ball_diameter)
    // This ensures cue strikes at edge to deflect ball toward pocket
    // ========================================================================
    static Point2D calculateGhostBallPosition(
        const Point2D& targetBallPos,
        const Point2D& pocketPos
    ) {
        Point2D toPocket = pocketPos - targetBallPos;
        double distance = std::sqrt(toPocket.square());
        
        if (distance < 0.1) return targetBallPos;
        
        // Normalize direction
        Point2D direction = toPocket * (1.0 / distance);
        
        // Ghost ball is positioned at ball_diameter away from center
        return targetBallPos - direction * BALL_DIAMETER;
    }
    
    // ========================================================================
    // EQUATION 4: Calculate collision accuracy (dot product)
    // Physics: accuracy = (A·B) / (|A||B|)
    // Measures alignment between cue direction and pocket direction
    // Range: [0, 1] where 1 = perfect alignment
    // ========================================================================
    static double calculateCollisionAccuracy(
        const Point2D& cueBallPos,
        const Point2D& targetBallPos,
        const Point2D& pocketPos
    ) {
        Point2D toTarget = targetBallPos - cueBallPos;
        Point2D toPocket = pocketPos - targetBallPos;
        
        double toTargetLen = std::sqrt(toTarget.square());
        double toPocketLen = std::sqrt(toPocket.square());
        
        if (toTargetLen < 0.1 || toPocketLen < 0.1) return 0.0;
        
        // Dot product
        double dotProduct = (toTarget.x * toPocket.x) + (toTarget.y * toPocket.y);
        
        // Normalize
        double accuracy = dotProduct / (toTargetLen * toPocketLen);
        
        return std::max(0.0, std::min(1.0, accuracy));
    }
    
    // ========================================================================
    // EQUATION 5: Calculate shot score (lower = better)
    // Score combines distance and accuracy with ball priority
    // ========================================================================
    static double calculateShotScore(
        double distanceToPocket,
        double shotDistance,
        double accuracy,
        BallType ballType,
        BallType myBallType,
        bool isMyBall
    ) {
        // Base score: distance + weighted accuracy
        double baseScore = distanceToPocket + shotDistance;
        baseScore *= (1.0 - (accuracy * 0.3));  // Reward high accuracy
        
        // Ball type multiplier
        if (isMyBall) {
            // MY BALLS: Strong priority (0.2x multiplier)
            baseScore *= 0.2;
        } else if (ballType != myBallType && ballType != EIGHT_BALL) {
            // OPPONENT BALLS: Lower priority (3.0x multiplier)
            baseScore *= 3.0;
        }
        
        return baseScore;
    }
    
    // ========================================================================
    // Validate pocket reachability
    // ========================================================================
    static bool isPocketReachable(double pocketDistance) {
        return pocketDistance >= MIN_POCKET_DIST && pocketDistance <= MAX_POCKET_DIST;
    }
    
    // ========================================================================
    // Validate ball won't scratch (cue ball won't be potted)
    // ========================================================================
    static bool validateCueBallSafety(const Prediction& pred) {
        return pred.guiData.balls[0].onTable;
    }
    
    // ========================================================================
    // Validate 8-ball won't be knocked prematurely
    // ========================================================================
    static bool validateEightBallSafety(const Prediction& pred, BallType myBallType) {
        auto& ball8 = pred.guiData.balls[8];
        
        // 8-ball must stay on table until it's your turn
        if (ball8.originalOnTable && !ball8.onTable && myBallType != EIGHT_BALL) {
            return false;  // 8-ball was potted prematurely!
        }
        
        return true;
    }
    
    // ========================================================================
    // Validate first ball hit matches player's ball type
    // ========================================================================
    static bool validateFirstHit(const Prediction& pred, BallType myBallType) {
        auto firstHit = pred.guiData.collision.firstHitBall;
        if (!firstHit) return false;
        
        // Determine first hit ball type
        BallType hitType = INVALID;
        if (firstHit->index == 8) {
            hitType = EIGHT_BALL;
        } else if (firstHit->classification == Ball::Classification::EIGHT_BALL) {
            hitType = EIGHT_BALL;
        } else if (firstHit->classification == Ball::Classification::ANY) {
            hitType = SOLIDS;  // Default
        } else if (firstHit->index >= 1 && firstHit->index <= 7) {
            hitType = SOLIDS;
        } else if (firstHit->index >= 9 && firstHit->index <= 15) {
            hitType = STRIPES;
        }
        
        // For Solids/Stripes mode: can hit any ball except 8-ball (until all yours are gone)
        if (myBallType == SOLIDS || myBallType == STRIPES) {
            if (hitType == EIGHT_BALL) return false;  // Can't hit 8-ball first
            return true;
        }
        
        // For 9-ball: must hit lowest numbered ball first
        // (handled separately in 9-ball logic)
        
        // For regular play: must hit your ball type first
        return hitType == myBallType;
    }
    
    // ========================================================================
    // Validate ball actually gets potted
    // ========================================================================
    static bool validateBallPocketed(const Prediction& pred) {
        for (int i = 1; i < pred.guiData.ballsCount; i++) {
            if (pred.guiData.balls[i].originalOnTable && !pred.guiData.balls[i].onTable) {
                return true;  // At least one ball was potted
            }
        }
        return false;
    }
};

// ============================================================================
// GAME STATE & HELPER FUNCTIONS
// ============================================================================
Point2D lastFailedCuePos = { -1000.0, -1000.0 };

BallType getBallType(int ballIndex) {
    if (ballIndex == 0) return CUE_BALL;
    if (ballIndex == 8) return EIGHT_BALL;
    if (ballIndex >= 1 && ballIndex <= 7) return SOLIDS;
    if (ballIndex >= 9 && ballIndex <= 15) return STRIPES;
    return INVALID;
}

BallType getPlayerBallType(Ball::Classification classification) {
    if (classification == Ball::Classification::ANY) return SOLIDS;  // Default
    if (classification == Ball::Classification::EIGHT_BALL) return EIGHT_BALL;
    // Could be extended for other classifications
    return SOLIDS;
}

// ============================================================================
// AUTOPLAY NAMESPACE
// ============================================================================
namespace AutoPlay {
    double lastSetAngle = 0.f;
    double lastSetPower = 0.f;
    bool bAutoPlaying = false;
    
    static FrictionProperties cachedFriction = {0.2, 0.0111, 0.025, 0.0014577259475218659, 196, 10.878, 9.8};

    enum State { IDLE, SCANNING, NOMINATING, EXECUTING } state = IDLE;
    enum ScanMode { FAST, SLOW } scan = FAST;
    
    double pendingShotPower = 0.f;
    double pendingShotAngle = 0.f;
    int nominationFrameCounter = 0;

    // ========================================================================
    // HELPER: Set aim angle
    // ========================================================================
    void setAimAngle(double angle) {
        lastSetAngle = angle;
        sharedGameManager.mVisualCue().mVisualGuide().mAimAngle(angle);
    }

    // ========================================================================
    // HELPER: Set shot power
    // ========================================================================
    void setShotPower(double power) {
        lastSetPower = power;
        sharedGameManager.mVisualCue().setShotPower(power);
    }

    // ========================================================================
    // HELPER: Execute shot
    // ========================================================================
    void takeShot(double angle, double power) {
        setAimAngle(angle);
        setShotPower(power);
        gPrediction->determineShotResult(false, angle, power);
        sharedGameManager.mVisualCue().mPower(ShotPowerToPower(power));
        M(void, libmain + 0x2dc0c58, void*)(F(void*, sharedGameManager + 0x3b0));
    }
    
    // ========================================================================
    // HELPER: Clear state
    // ========================================================================
    void ClearState() {
        g_CurrentCandidate.idx = -1;
        lastFailedCuePos = { -1000.0, -1000.0 };
    }
    
    // ========================================================================
    // MAIN: Execute shot with nomination
    // ========================================================================
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
    
    // ========================================================================
    // SCAN FAST: Quick scan with physics-optimized angles
    // ========================================================================
    void ScanFast(double angleStep = ANGLE_STEP_FAST) {
        if (g_CurrentCandidate.idx != -1) return;
        if (gPrediction->guiData.balls[0].initialPosition == lastFailedCuePos) return;

        Ball::Classification playerClass = sharedGameManager.getPlayerClassification();
        BallType myBallType = getPlayerBallType(playerClass);
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        
        std::vector<Candidate> candidates;
        auto pockets = getPockets();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        bool isNineBallGame = myBallType == SOLIDS;  // Placeholder for 9-ball detection
        
        // ====================================================================
        // ITERATE: All balls on table
        // ====================================================================
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            auto& ball = gPrediction->guiData.balls[i];
            if (!ball.originalOnTable) continue;
            
            BallType ballType = getBallType(i);
            bool isMyBall = (ballType == myBallType) && (ballType != EIGHT_BALL);
            
            // Skip if wrong ball type (for strict mode)
            bool isCandidate = false;
            if (isMyBall) {
                isCandidate = true;  // Always try MY balls first
            } else if (ballType != EIGHT_BALL && ballType != CUE_BALL) {
                isCandidate = true;  // Opponent balls as fallback
            }
            
            if (!isCandidate) continue;

            // ================================================================
            // ITERATE: All pockets
            // ================================================================
            for (int pocketIdx = 0; pocketIdx < pockets.size(); pocketIdx++) {
                if (nominatedPocket < 6 && pocketIdx != nominatedPocket) continue;

                Point2D pocket = pockets[pocketIdx];
                
                // EQUATION 1: Distance validation
                Point2D toPocket = pocket - ball.initialPosition;
                double pocketDistance = std::sqrt(toPocket.square());
                
                if (!PhysicsEngine::isPocketReachable(pocketDistance)) continue;
                
                // EQUATION 3: Ghost ball position for collision
                Point2D ghostBallPos = PhysicsEngine::calculateGhostBallPosition(
                    ball.initialPosition,
                    pocket
                );
                
                // EQUATION 2: Calculate trajectory angle
                Point2D shotLine = ghostBallPos - cueBall.initialPosition;
                double shotDistance = std::sqrt(shotLine.square());
                if (shotDistance < 0.1) continue;
                
                double angle = PhysicsEngine::calculateTrajectoryAngle(
                    cueBall.initialPosition,
                    ghostBallPos
                );
                
                // EQUATION 4: Collision accuracy
                double accuracy = PhysicsEngine::calculateCollisionAccuracy(
                    cueBall.initialPosition,
                    ball.initialPosition,
                    pocket
                );
                
                // EQUATION 5: Shot score
                double score = PhysicsEngine::calculateShotScore(
                    pocketDistance,
                    shotDistance,
                    accuracy,
                    ballType,
                    myBallType,
                    isMyBall
                );
                
                // EQUATION 1: Calculate power
                double power = PhysicsEngine::calculatePowerForDistance(
                    pocketDistance,
                    cachedFriction
                );
                
                candidates.push_back({i, angle, score, pocketIdx, power});
            }
        }
        
        std::sort(candidates.begin(), candidates.end());
        
        bool foundShot = false;
        
        // ====================================================================
        // VALIDATE: Each candidate
        // ====================================================================
        for (const auto& cand : candidates) {
            double angle = NumberUtils::normalizeDoublePrecision(normalizeAngle(cand.angle));
            gPrediction->determineShotResult(true, angle, cand.power, sharedGameManager.getShotSpin(), cand);
            
            // Safety checks
            if (!PhysicsEngine::validateCueBallSafety(*gPrediction)) continue;
            if (!PhysicsEngine::validateEightBallSafety(*gPrediction, myBallType)) continue;
            if (!PhysicsEngine::validateFirstHit(*gPrediction, myBallType)) continue;
            if (!PhysicsEngine::validateBallPocketed(*gPrediction)) continue;
            
            // Target ball checks
            if (gPrediction->guiData.balls[cand.idx].onTable) continue;
            if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) continue;
            
            LOGI("AutoPlay: Found angle %f with power %f", angle, cand.power);
            g_CurrentCandidate = cand;
            foundShot = true;
            Shoot(angle, cand.power);
            break;
        }

        if (!foundShot) {
            lastFailedCuePos = cueBall.initialPosition;
            LOGI("AutoPlay: ScanFast failed, trying ScanSlow");
            scan = SLOW;
        }
    }

    // ========================================================================
    // SCAN SLOW: Exhaustive angle search
    // ========================================================================
    void ScanSlow(double angleStep = ANGLE_STEP_SLOW) {
        static double currentScanAngle = 0.0;
        static bool isScanning = false;
        static Point2D lastScanCuePos = { -1000.0, -1000.0 };

        if (g_CurrentCandidate.idx != -1) return;
        
        if (!isScanning || gPrediction->guiData.balls[0].initialPosition != lastScanCuePos) {
            currentScanAngle = 0.0;
            isScanning = true;
            lastScanCuePos = gPrediction->guiData.balls[0].initialPosition;
        }

        Ball::Classification playerClass = sharedGameManager.getPlayerClassification();
        BallType myBallType = getPlayerBallType(playerClass);
        uint nominatedPocket = sharedGameManager.getNominatedPocket();
        auto& cueBall = gPrediction->guiData.balls[0];
        
        int steps = 0;
        int stepsPerFrame = (int)(20 * GameSpeed::getAnimationMultiplier());
        
        // ====================================================================
        // ITERATE: All angles
        // ====================================================================
        while (steps < stepsPerFrame && currentScanAngle < maxAngle) {
            double angle = currentScanAngle;
            currentScanAngle += angleStep;
            steps++;

            // Strategic power levels
            std::vector<double> powers = {666.0, 500.0, 350.0, 200.0, 100.0};
            
            for (double power : powers) {
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
                
                // Safety checks FIRST
                if (!PhysicsEngine::validateCueBallSafety(*gPrediction)) continue;
                if (!PhysicsEngine::validateEightBallSafety(*gPrediction, myBallType)) continue;
                if (!PhysicsEngine::validateFirstHit(*gPrediction, myBallType)) continue;
                if (!PhysicsEngine::validateBallPocketed(*gPrediction)) continue;
                
                // Find what was potted
                int targetIdx = -1;
                for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
                    auto& ball = gPrediction->guiData.balls[i];
                    if (!ball.originalOnTable || ball.onTable) continue;

                    BallType ballType = getBallType(i);
                    bool isMyBall = (ballType == myBallType) && (ballType != EIGHT_BALL);
                    
                    bool isValid = isMyBall || (ballType != EIGHT_BALL && ballType != CUE_BALL);
                    if (nominatedPocket < 6 && ball.pocketIndex != nominatedPocket) isValid = false;
                    
                    if (isValid) { targetIdx = i; break; }
                }

                if (targetIdx == -1) continue;

                LOGI("AutoPlaySlow: Found angle %f power %f", angle, power);
                g_CurrentCandidate.idx = targetIdx;
                g_CurrentCandidate.angle = angle;
                g_CurrentCandidate.power = power;
                g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;
                Shoot(angle, power);
                return;
            }
        }

        if (currentScanAngle >= maxAngle) {
            LOGI("AutoPlaySlow: Exhaustive scan complete");
            isScanning = false;
            currentScanAngle = 0.0;
            state = IDLE;
        }
    }

    // ========================================================================
    // UI: Draw toggle button
    // ========================================================================
    void DrawToggleButton() {
        ImGuiIO& io = GetIO();
        float button_size = ImGui::GetFrameHeight() * 2.3f;
        float windowWidth = button_size + GetStyle().WindowPadding.x * 2;
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

    // ========================================================================
    // CHECK: Is animation active?
    // ========================================================================
    bool isAnimationActive() {
        auto visualCue = sharedGameManager.mVisualCue();
        if (!visualCue) return true;
        
        auto _powerBarView = F(ptr, visualCue + 0x510);
        if (!_powerBarView) return true;

        auto activeAction = M(ptr, libmain + 0x2de6f30, ptr)(_powerBarView);
        return activeAction != nullptr;
    }
    
    // ========================================================================
    // MAIN: Update loop
    // ========================================================================
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
            if (scan == FAST) {
                ScanFast(ANGLE_STEP_FAST);
            } else {
                DrawEightBallLoading(GetForegroundDrawList());
                ScanSlow(ANGLE_STEP_SLOW);
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
