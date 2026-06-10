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
// PHYSICS ENGINE - CORRECTED FOR PROPER BALL COLLISION
// ============================================================================
struct PhysicsEngine {
    static constexpr double BALL_DIAMETER = 2.0 * Physics::BALL_RADIUS;
    static constexpr double GRAVITY = 9.81;
    
    // ========================================================================
    // EQUATION 1: Calculate cue power to transfer momentum to target ball
    // Physics: Elastic collision - cue ball transfers energy to target
    // v_target = (2 * m_cue / (m_cue + m_target)) * v_cue
    // Simplified: v_target = v_cue (equal mass)
    // Required: Power = Distance * Friction_Coefficient
    // ========================================================================
    static double calculatePowerForTargetToPocket(
        double cueToBallDist,      // Distance from cue to target ball
        double ballToPocketDist,   // Distance from target ball to pocket (ACTUAL SHOT)
        const FrictionProperties& friction
    ) {
        // The target ball needs to travel from its position to the pocket
        // This is the ACTUAL distance the ball must cover
        if (ballToPocketDist < 1.0) return 100.0;
        
        // Friction deceleration: a = -mu * g
        double friction_coeff = friction._velocityReductionRollingFactor;
        double deceleration = GRAVITY * friction_coeff;
        
        // CORRECTED: Power needed for TARGET BALL to reach pocket
        // v^2 = 2 * a * s  =>  v = sqrt(2 * a * s)
        double requiredVelocity = std::sqrt(2.0 * deceleration * ballToPocketDist);
        
        // Add extra power to overcome collision loss (elastic collision efficiency ~90%)
        double powerWithCollisionLoss = requiredVelocity / 0.9;  // Account for energy loss
        
        // Map to power scale (0-666)
        double power = powerWithCollisionLoss / 1.0;
        return std::min(std::max(power, 100.0), 666.0);
    }
    
    // ========================================================================
    // EQUATION 2: Calculate angle for cue to hit target at POCKET ANGLE
    // Physics: Cue -> Target -> Pocket (straight line from target to pocket)
    // ========================================================================
    static double calculateAngleToPocket(
        const Point2D& cueBallPos,
        const Point2D& targetBallPos,
        const Point2D& pocketPos
    ) {
        // CORRECTED: Calculate angle FROM TARGET BALL TO POCKET
        // This is what matters - where the ball GOES after being hit
        Point2D ballToPocket = pocketPos - targetBallPos;
        double angle = std::atan2(ballToPocket.y, ballToPocket.x);
        
        // Normalize to [0, 2π)
        if (angle < 0) angle += TWO_PI;
        
        return angle;
    }
    
    // ========================================================================
    // EQUATION 3: Calculate collision point (where cue hits target ball)
    // Physics: For ball to go toward pocket, cue hits opposite side
    // Collision point = target_center - (direction_to_pocket * radius)
    // ========================================================================
    static Point2D calculateCollisionPoint(
        const Point2D& targetBallPos,
        const Point2D& pocketPos
    ) {
        Point2D toPocket = pocketPos - targetBallPos;
        double distance = std::sqrt(toPocket.square());
        
        if (distance < 0.1) return targetBallPos;
        
        // Normalize direction
        Point2D direction = toPocket * (1.0 / distance);
        
        // Collision point: Hit the OPPOSITE side to propel toward pocket
        // This is where the cue ball should aim
        return targetBallPos - direction * Physics::BALL_RADIUS;
    }
    
    // ========================================================================
    // EQUATION 4: Calculate shot accuracy (how direct is the path?)
    // Physics: Accuracy = alignment between target->pocket and cue->target
    // Direct path = 1.0, off-angle = 0.0
    // ========================================================================
    static double calculateShotAccuracy(
        const Point2D& cueBallPos,
        const Point2D& targetBallPos,
        const Point2D& pocketPos
    ) {
        // Vector from cue to target
        Point2D cueToTarget = targetBallPos - cueBallPos;
        // Vector from target to pocket
        Point2D targetToPocket = pocketPos - targetBallPos;
        
        double cueToTargetLen = std::sqrt(cueToTarget.square());
        double targetToPocketLen = std::sqrt(targetToPocket.square());
        
        if (cueToTargetLen < 0.1 || targetToPocketLen < 0.1) return 0.0;
        
        // Dot product: measures alignment
        double dotProduct = (cueToTarget.x * targetToPocket.x) + 
                           (cueToTarget.y * targetToPocket.y);
        
        // Normalize to [0, 1]
        double accuracy = dotProduct / (cueToTargetLen * targetToPocketLen);
        
        return std::max(0.0, std::min(1.0, accuracy));
    }
    
    // ========================================================================
    // EQUATION 5: Calculate shot score (lower = better)
    // Score: (distance_to_pocket) * (1 - accuracy) * ball_priority
    // ========================================================================
    static double calculateShotScore(
        double targetBallToPocketDist,  // ACTUAL distance ball travels
        double accuracy,
        BallType ballType,
        BallType myBallType,
        bool isMyBall
    ) {
        // Base: favor close pockets
        double baseScore = targetBallToPocketDist;
        
        // Reward accuracy (direct shots are better)
        baseScore *= (1.0 - (accuracy * 0.5));  // High accuracy = lower score
        
        // Ball type priority
        if (isMyBall) {
            // MY BALLS: 0.2x multiplier (STRONG PRIORITY - chosen first)
            baseScore *= 0.2;
        } else if (ballType != EIGHT_BALL) {
            // OPPONENT BALLS: 3.0x multiplier (LOW PRIORITY - fallback only)
            baseScore *= 3.0;
        }
        
        return baseScore;
    }
    
    // ========================================================================
    // Validate pocket is reachable from target ball
    // ========================================================================
    static bool isPocketReachable(double targetBallToPocketDist) {
        return targetBallToPocketDist >= MIN_POCKET_DIST && 
               targetBallToPocketDist <= MAX_POCKET_DIST;
    }
    
    // ========================================================================
    // Validate cue ball won't scratch (won't be potted)
    // ========================================================================
    static bool validateCueBallSafety(const Prediction& pred) {
        return pred.guiData.balls[0].onTable;  // Cue ball still on table
    }
    
    // ========================================================================
    // Validate 8-ball won't be potted prematurely
    // ========================================================================
    static bool validateEightBallSafety(const Prediction& pred, BallType myBallType) {
        auto& ball8 = pred.guiData.balls[8];
        
        // 8-ball must stay on table until it's your turn (you've cleared all yours)
        if (ball8.originalOnTable && !ball8.onTable && myBallType != EIGHT_BALL) {
            return false;  // 8-ball was knocked in prematurely!
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
        } else if (firstHit->index >= 1 && firstHit->index <= 7) {
            hitType = SOLIDS;
        } else if (firstHit->index >= 9 && firstHit->index <= 15) {
            hitType = STRIPES;
        }
        
        // For Solids/Stripes: can hit any ball except 8-ball
        if (myBallType == SOLIDS || myBallType == STRIPES) {
            if (hitType == EIGHT_BALL) return false;  // Can't hit 8-ball first
            return true;
        }
        
        return true;
    }
    
    // ========================================================================
    // Validate target ball actually gets potted (not opponent ball)
    // ========================================================================
    static bool validateTargetBallPocketed(const Prediction& pred, int targetIdx) {
        // CORRECTED: Check that ONLY the target ball was potted
        // Not opponent balls by accident
        auto& targetBall = pred.guiData.balls[targetIdx];
        
        return targetBall.originalOnTable && !targetBall.onTable;
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
    if (classification == Ball::Classification::ANY) return SOLIDS;
    if (classification == Ball::Classification::EIGHT_BALL) return EIGHT_BALL;
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
    // SCAN FAST: Quick scan with physics-corrected angle calculation
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
        
        // ====================================================================
        // ITERATE: All balls on table
        // ====================================================================
        for (int i = 1; i < gPrediction->guiData.ballsCount; i++) {
            auto& ball = gPrediction->guiData.balls[i];
            if (!ball.originalOnTable) continue;
            
            BallType ballType = getBallType(i);
            bool isMyBall = (ballType == myBallType) && (ballType != EIGHT_BALL);
            
            // Candidate check: try MY balls first, then opponent balls
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
                
                // CORRECTED: Distance from TARGET BALL to POCKET (not cue to target)
                Point2D ballToPocket = pocket - ball.initialPosition;
                double ballToPocketDist = std::sqrt(ballToPocket.square());
                
                if (!PhysicsEngine::isPocketReachable(ballToPocketDist)) continue;
                
                // CORRECTED: Calculate where cue should hit the target ball
                // So that target ball goes DIRECTLY toward pocket
                Point2D collisionPoint = PhysicsEngine::calculateCollisionPoint(
                    ball.initialPosition,
                    pocket
                );
                
                // Calculate angle from cue to collision point
                Point2D cueToCollision = collisionPoint - cueBall.initialPosition;
                double cueToCollisionDist = std::sqrt(cueToCollision.square());
                if (cueToCollisionDist < 0.1) continue;
                
                double angle = PhysicsEngine::calculateAngleToPocket(
                    cueBall.initialPosition,
                    ball.initialPosition,
                    pocket
                );
                
                // Calculate accuracy
                double accuracy = PhysicsEngine::calculateShotAccuracy(
                    cueBall.initialPosition,
                    ball.initialPosition,
                    pocket
                );
                
                // Calculate score
                double score = PhysicsEngine::calculateShotScore(
                    ballToPocketDist,
                    accuracy,
                    ballType,
                    myBallType,
                    isMyBall
                );
                
                // CORRECTED: Calculate power needed for TARGET BALL to reach pocket
                double power = PhysicsEngine::calculatePowerForTargetToPocket(
                    cueToCollisionDist,
                    ballToPocketDist,
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
            if (!PhysicsEngine::validateTargetBallPocketed(*gPrediction, cand.idx)) continue;
            
            // Verify target ball is in correct pocket
            if (gPrediction->guiData.balls[cand.idx].pocketIndex != cand.pocketIndex) continue;
            
            LOGI("AutoPlay: FAST - Ball %d angle %f power %f", cand.idx, angle, cand.power);
            g_CurrentCandidate = cand;
            foundShot = true;
            Shoot(angle, cand.power);
            break;
        }

        if (!foundShot) {
            lastFailedCuePos = cueBall.initialPosition;
            LOGI("AutoPlay: ScanFast failed, switching to ScanSlow");
            scan = SLOW;
        }
    }

    // ========================================================================
    // SCAN SLOW: Exhaustive angle search for any possible shot
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

            // Strategic power levels for testing
            std::vector<double> powers = {666.0, 500.0, 350.0, 200.0, 100.0};
            
            for (double power : powers) {
                gPrediction->determineShotResult(true, angle, power, sharedGameManager.getShotSpin());
                
                // Safety checks FIRST
                if (!PhysicsEngine::validateCueBallSafety(*gPrediction)) continue;
                if (!PhysicsEngine::validateEightBallSafety(*gPrediction, myBallType)) continue;
                if (!PhysicsEngine::validateFirstHit(*gPrediction, myBallType)) continue;
                
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

                LOGI("AutoPlay: SLOW - Ball %d angle %f power %f", targetIdx, angle, power);
                g_CurrentCandidate.idx = targetIdx;
                g_CurrentCandidate.angle = angle;
                g_CurrentCandidate.power = power;
                g_CurrentCandidate.pocketIndex = gPrediction->guiData.balls[targetIdx].pocketIndex;
                Shoot(angle, power);
                return;
            }
        }

        if (currentScanAngle >= maxAngle) {
            LOGI("AutoPlaySlow: Exhaustive scan complete, no shot found");
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
