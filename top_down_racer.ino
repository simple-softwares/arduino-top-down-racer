/*
 * =====================================================
 *  TOP-DOWN RACER — Nokia 5110 (PCD8544) Edition
 *  Display : Adafruit_PCD8544  84x48 px
 *
 *  Wiring:
 *    Display  CLK=7  DIN=6  DC=5  CE=4  RST=3
 *    Keyes AD Key:
 *      OUT → A0  |  VCC → 5V  |  GND → GND
 *
 *  Calibrated ADC thresholds (your module):
 *    SW1=0   → LEFT
 *    SW2=28  → BRAKE  (slow down)
 *    SW3=81  → NITRO  (speed burst!)
 *    SW4=154 → RIGHT
 *    SW5=332 → START / SELECT
 *
 *  Road: 3 lanes across 60px (centred), shoulders on sides
 *  Player car: lane-based X, fixed near bottom
 *  Traffic: 3 simultaneous cars, random lanes, scroll down
 *  Speed increases every 10 points
 *  Nitro: 3 charges shown as pips — refills 1 charge every 15s
 * =====================================================
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_PCD8544.h>

Adafruit_PCD8544 display = Adafruit_PCD8544(7, 6, 5, 4, 3);

#define ANALOG_PIN A0
#define BUZZER_PIN 10 // if using buzzer


// ── Calibrated thresholds ─────────────────────────────
#define THR_SW1  14
#define THR_SW2  54
#define THR_SW3 117
#define THR_SW4 243
#define THR_SW5 677

#define KEY_NONE   0
#define KEY_LEFT   1
#define KEY_BRAKE  2
#define KEY_NITRO  3
#define KEY_RIGHT  4
#define KEY_SELECT 5

// ── Screen ────────────────────────────────────────────
#define SCR_W 84
#define SCR_H 48

// ── Road layout ───────────────────────────────────────
// Road occupies x=12..71 (60px wide), 3 lanes of 20px each
#define ROAD_L     12
#define ROAD_R     72
#define ROAD_W     60
#define LANE_W     20
#define NUM_LANES   3

// Lane centre X values
const int LANE_CX[NUM_LANES] = { 22, 42, 62 };

// Lane divider X values (dashed lines)
const int DIV_X[2] = { 32, 52 };

// ── Player car ────────────────────────────────────────
#define CAR_W       8
#define CAR_H      12
#define PLAYER_Y   (SCR_H - CAR_H - 3)   // 33

int  playerLane;   // 0=left 1=mid 2=right
int  playerX;      // actual pixel X (interpolates toward lane centre)

// ── Speed / score ─────────────────────────────────────
float baseSpeed;        // road scroll speed (px/frame)
float currentSpeed;     // actual speed (modified by brake/nitro)
int   score;
int   hiScore;
unsigned long lastScoreTick; // ms since last score increment

// ── Nitro ─────────────────────────────────────────────
#define MAX_NITRO      3
#define NITRO_DURATION 40   // frames of boost
#define NITRO_RECHARGE 600  // frames per charge recharge (~15s at 40fps)
int  nitroCharges;
int  nitroFrames;           // frames remaining in current burst
int  nitroRechargeTimer;    // counts up to NITRO_RECHARGE

// ── Road dashes ───────────────────────────────────────
#define NUM_DASHES 6
float dashY[NUM_DASHES];   // Y position of each dash segment

// ── Road edge lines (animated) ────────────────────────
float edgeY;

// ── Traffic cars ──────────────────────────────────────
#define NUM_TRAFFIC 3
#define TCAR_W       8
#define TCAR_H      10

struct TrafficCar {
  float y;
  int   lane;
  bool  active;
};
TrafficCar traffic[NUM_TRAFFIC];
int  trafficSpawnTimer;
int  trafficSpawnInterval;  // frames between spawns

// ── Crash animation ───────────────────────────────────
#define CRASH_FRAMES 30
int  crashTimer;            // counts down when crashing
int  crashX, crashY;        // centre of explosion

// ── Game state ────────────────────────────────────────
#define STATE_START    0
#define STATE_PLAYING  1
#define STATE_CRASH    2
#define STATE_GAMEOVER 3

int gameState;

// ── Button state ──────────────────────────────────────
int  lastKey = KEY_NONE;
bool heldLeft, heldRight, heldBrake;
bool justNitro, justSelect;

// =====================================================
//  BUTTON READ
// =====================================================
int readKey() {
  int v = analogRead(ANALOG_PIN);
  if      (v <= THR_SW1) return KEY_LEFT;
  else if (v <= THR_SW2) return KEY_BRAKE;
  else if (v <= THR_SW3) return KEY_NITRO;
  else if (v <= THR_SW4) return KEY_RIGHT;
  else if (v <= THR_SW5) return KEY_SELECT;
  else                   return KEY_NONE;
}

void updateButtons() {
  int cur = readKey();
  heldLeft   = (cur == KEY_LEFT);
  heldRight  = (cur == KEY_RIGHT);
  heldBrake  = (cur == KEY_BRAKE);
  justNitro  = (cur == KEY_NITRO  && lastKey != KEY_NITRO);
  justSelect = (cur == KEY_SELECT && lastKey != KEY_SELECT);
  lastKey = cur;
}

// =====================================================
//  DRAW SPRITES
// =====================================================

// Player car (8×12) — sleek top-down shape
void drawPlayerCar(int cx, int y) {
  int x = cx - CAR_W/2;
  // Body
  display.fillRect(x+1, y,     CAR_W-2, CAR_H,   BLACK);
  // Windscreen indent (white stripe)
  display.fillRect(x+2, y+1,   CAR_W-4, 3,        WHITE);
  // Wheels (4 corner rectangles)
  display.fillRect(x,   y+1,   2, 3, BLACK);
  display.fillRect(x+CAR_W-2, y+1, 2, 3, BLACK);
  display.fillRect(x,   y+CAR_H-4, 2, 3, BLACK);
  display.fillRect(x+CAR_W-2, y+CAR_H-4, 2, 3, BLACK);
}

// Traffic car (8×10) — slightly different shape
void drawTrafficCar(int cx, int y) {
  int x = cx - TCAR_W/2;
  display.fillRect(x+1, y,     TCAR_W-2, TCAR_H, BLACK);
  display.fillRect(x+2, y+TCAR_H-4, TCAR_W-4, 3, WHITE); // rear window
  display.fillRect(x,   y+2,   2, 3, BLACK);
  display.fillRect(x+TCAR_W-2, y+2, 2, 3, BLACK);
  display.fillRect(x,   y+TCAR_H-4, 2, 3, BLACK);
  display.fillRect(x+TCAR_W-2, y+TCAR_H-4, 2, 3, BLACK);
}

// Crash explosion (expanding cross + ring)
void drawExplosion(int cx, int cy, int frame) {
  // frame goes 0→CRASH_FRAMES, animate outward
  int r = frame / 4 + 1;
  // Cross arms
  display.drawFastHLine(cx - r,     cy,     r*2+1, BLACK);
  display.drawFastVLine(cx,         cy - r, r*2+1, BLACK);
  // Corner sparks
  if (r >= 2) {
    display.drawPixel(cx - r + 1, cy - r + 1, BLACK);
    display.drawPixel(cx + r - 1, cy - r + 1, BLACK);
    display.drawPixel(cx - r + 1, cy + r - 1, BLACK);
    display.drawPixel(cx + r - 1, cy + r - 1, BLACK);
  }
  // Flicker: invert every other frame
  if ((frame % 4) < 2) {
    display.fillRect(cx-1, cy-1, 3, 3, WHITE);
  }
}

// Nitro flame behind player (when nitro active)
void drawNitroFlame(int cx, int y) {
  // Small flame triangles at bottom of car
  int bx = cx - CAR_W/2;
  display.drawPixel(cx - 2, y + CAR_H,     BLACK);
  display.drawPixel(cx - 2, y + CAR_H + 1, BLACK);
  display.drawPixel(cx,     y + CAR_H,     BLACK);
  display.drawPixel(cx,     y + CAR_H + 1, BLACK);
  display.drawPixel(cx,     y + CAR_H + 2, BLACK);
  display.drawPixel(cx + 2, y + CAR_H,     BLACK);
  display.drawPixel(cx + 2, y + CAR_H + 1, BLACK);
  (void)bx;
}

// =====================================================
//  DRAW ROAD
// =====================================================
void drawRoad() {
  // Shoulders (solid side strips)
  display.fillRect(0,       0, ROAD_L,          SCR_H, BLACK);
  display.fillRect(ROAD_R,  0, SCR_W - ROAD_R,  SCR_H, BLACK);

  // Shoulder edge lines (white border on road side)
  display.drawFastVLine(ROAD_L,     0, SCR_H, WHITE);
  display.drawFastVLine(ROAD_R - 1, 0, SCR_H, WHITE);

  // Animated lane dashes
  for (int d = 0; d < NUM_DASHES; d++) {
    int yd = (int)dashY[d];
    // Two divider lines
    for (int i = 0; i < 2; i++) {
      display.drawFastVLine(DIV_X[i], yd,     4, BLACK);
      // Gap handled by white road background
    }
  }
}

// =====================================================
//  SPAWN TRAFFIC
// =====================================================
void spawnTraffic() {
  // Find an inactive slot
  for (int i = 0; i < NUM_TRAFFIC; i++) {
    if (!traffic[i].active) {
      // Pick a random lane, not the same as an already-near car
      int lane;
      int attempts = 0;
      do {
        lane = random(0, NUM_LANES);
        attempts++;
        if (attempts > 10) break;
        // Check no other car is in this lane near the top
        bool occupied = false;
        for (int j = 0; j < NUM_TRAFFIC; j++) {
          if (j == i) continue;
          if (traffic[j].active && traffic[j].lane == lane && traffic[j].y < 20)
            occupied = true;
        }
        if (!occupied) break;
      } while (true);

      traffic[i].y      = -(float)TCAR_H;
      traffic[i].lane   = lane;
      traffic[i].active = true;
      return;
    }
  }
}

// =====================================================
//  RESET
// =====================================================
void resetGame() {
  playerLane    = 1;
  playerX       = LANE_CX[1];
  baseSpeed     = 1.5f;
  currentSpeed  = baseSpeed;
  score         = 0;
  lastScoreTick = millis();

  nitroCharges       = MAX_NITRO;
  nitroFrames        = 0;
  nitroRechargeTimer = 0;

  // Initialise road dashes evenly spaced
  for (int d = 0; d < NUM_DASHES; d++)
    dashY[d] = (SCR_H / NUM_DASHES) * d;
  edgeY = 0;

  for (int i = 0; i < NUM_TRAFFIC; i++)
    traffic[i].active = false;

  trafficSpawnTimer    = 0;
  trafficSpawnInterval = 50;

  crashTimer = 0;
}

// =====================================================
//  SETUP
// =====================================================
void setup() {
  display.begin();
  display.setContrast(57);
  display.setTextSize(1);
  display.setTextColor(BLACK);
  pinMode(ANALOG_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT); // if using buzzer

  randomSeed(analogRead(A1));
  hiScore   = 0;
  gameState = STATE_START;
  resetGame();
}

// =====================================================
//  LOOP
// =====================================================
void loop() {
  updateButtons();

  // ===================================================
  //  START SCREEN
  // ===================================================
  if (gameState == STATE_START) {
    display.clearDisplay();
    // Road backdrop
    display.fillRect(0, 0, ROAD_L, SCR_H, BLACK);
    display.fillRect(ROAD_R, 0, SCR_W-ROAD_R, SCR_H, BLACK);
    display.drawFastVLine(ROAD_L,   0, SCR_H, WHITE);
    display.drawFastVLine(ROAD_R-1, 0, SCR_H, WHITE);

    display.setCursor(16, 4);  display.print("TOP-DOWN");
    display.setCursor(22, 13); display.print("RACER");

    // Mini car on title screen
    drawPlayerCar(42, 24);

    display.setCursor(14, 38); display.print("SW5 = START");
    display.display();

    // if (justSelect) { resetGame(); gameState = STATE_PLAYING; }
     if (justSelect) {
    // Rising start jingle: three quick ascending tones
    tone(BUZZER_PIN, 523, 80);  delay(90);   // C5
    tone(BUZZER_PIN, 659, 80);  delay(90);   // E5
    tone(BUZZER_PIN, 784, 120); delay(130);  // G5
    noTone(BUZZER_PIN);
    resetGame();
    gameState = STATE_PLAYING;
  }

    delay(30);
    return;
  }

  // ===================================================
  //  GAME OVER SCREEN
  // ===================================================
  if (gameState == STATE_GAMEOVER) {
    if (score > hiScore) hiScore = score;
    display.clearDisplay();
    display.setCursor(13, 3);  display.print("GAME OVER");
    display.setCursor(8,  16); display.print("Score:"); display.print(score);
    display.setCursor(8,  25); display.print("Best: "); display.print(hiScore);
    display.setCursor(5,  38); display.print("SW5 to retry");
    display.display();
    if (justSelect) { gameState = STATE_START; }
    delay(30);
    return;
  }

  // ===================================================
  //  CRASH ANIMATION
  // ===================================================
  if (gameState == STATE_CRASH) {
    display.clearDisplay();
    drawRoad();
    drawExplosion(crashX, crashY, CRASH_FRAMES - crashTimer);
    display.display();
    crashTimer--;
    if (crashTimer <= 0) gameState = STATE_GAMEOVER;
    delay(30);
    return;
  }

  // ===================================================
  //  PLAYING
  // ===================================================

  // ── Speed up over time ───────────────────────────
  // Base speed increases every 10 score points
  baseSpeed = 1.5f + (score / 10) * 0.3f;
  if (baseSpeed > 5.0f) baseSpeed = 5.0f;
  trafficSpawnInterval = max(20, 50 - score / 2);

  // ── Nitro recharge ───────────────────────────────
  if (nitroCharges < MAX_NITRO) {
    nitroRechargeTimer++;
    if (nitroRechargeTimer >= NITRO_RECHARGE) {
      nitroCharges++;
      nitroRechargeTimer = 0;
    }
  }

  // ── Nitro activation ─────────────────────────────
  if (justNitro && nitroCharges > 0 && nitroFrames == 0) {
    nitroFrames = NITRO_DURATION;
    nitroCharges--;
  }

  // ── Current speed ────────────────────────────────
  if (nitroFrames > 0) {
    currentSpeed = baseSpeed * 2.8f;
    if (currentSpeed > 8.0f) currentSpeed = 8.0f;
    nitroFrames--;
  } else if (heldBrake) {
    currentSpeed = baseSpeed * 0.35f;
    if (currentSpeed < 0.4f) currentSpeed = 0.4f;
  } else {
    currentSpeed = baseSpeed;
  }

  // ── Lane change ──────────────────────────────────
  // Only respond to fresh press (edge-triggered) to avoid jitter
  static bool wasLeft = false, wasRight = false;
  bool doLeft  = (heldLeft  && !wasLeft);
  bool doRight = (heldRight && !wasRight);
  wasLeft  = heldLeft;
  wasRight = heldRight;

  if (doLeft  && playerLane > 0)             playerLane--;
  if (doRight && playerLane < NUM_LANES - 1) playerLane++;

  // Smoothly slide playerX toward target lane centre
  int targetX = LANE_CX[playerLane];
  if (playerX < targetX) { playerX += 3; if (playerX > targetX) playerX = targetX; }
  if (playerX > targetX) { playerX -= 3; if (playerX < targetX) playerX = targetX; }

  // ── Scroll road dashes ───────────────────────────
  float dashSpacing = SCR_H / (float)NUM_DASHES;
  for (int d = 0; d < NUM_DASHES; d++) {
    dashY[d] += currentSpeed;
    if (dashY[d] > SCR_H) dashY[d] -= SCR_H;
  }

  // ── Score (distance-based) ───────────────────────
  unsigned long now = millis();
  // Score ticks faster when going faster
  int msPerPoint = max(100, (int)(600.0f / currentSpeed));
  if (now - lastScoreTick >= (unsigned long)msPerPoint) {
    score++;
    lastScoreTick = now;
  }

  // ── Traffic spawn ────────────────────────────────
  trafficSpawnTimer++;
  if (trafficSpawnTimer >= trafficSpawnInterval) {
    trafficSpawnTimer = 0;
    spawnTraffic();
  }

  // ── Move traffic ─────────────────────────────────
  for (int i = 0; i < NUM_TRAFFIC; i++) {
    if (!traffic[i].active) continue;
    traffic[i].y += currentSpeed * 0.85f; // slightly slower than road scroll
    if (traffic[i].y > SCR_H + TCAR_H) traffic[i].active = false;
  }

  // ── Collision detection ──────────────────────────
  int px = playerX - CAR_W/2;
  int py = PLAYER_Y;
  for (int i = 0; i < NUM_TRAFFIC; i++) {
    if (!traffic[i].active) continue;
    int tx = LANE_CX[traffic[i].lane] - TCAR_W/2;
    int ty = (int)traffic[i].y;
    // AABB collision (with 1px shrink for forgiveness)
    if (px + CAR_W - 1 > tx + 1 && px + 1 < tx + TCAR_W - 1 &&
        py + CAR_H - 1 > ty + 1 && py + 1 < ty + TCAR_H - 1) {
    
      // CRASH! NO BUZZER
      // crashX     = playerX;
      // crashY     = PLAYER_Y + CAR_H/2;
      // crashTimer = CRASH_FRAMES;
      // gameState  = STATE_CRASH;

    // if using buzzer
    crashX     = playerX;
    crashY     = PLAYER_Y + CAR_H/2;
    crashTimer = CRASH_FRAMES;
    gameState  = STATE_CRASH;
    // Descending crash sound: three dropping tones
    tone(BUZZER_PIN, 600, 80);  delay(70);
    tone(BUZZER_PIN, 350, 80);  delay(70);
    tone(BUZZER_PIN, 180, 200); delay(210);
    noTone(BUZZER_PIN);
      delay(30);
      return;
    }
  }

  // ===================================================
  //  DRAW FRAME
  // ===================================================
  display.clearDisplay();

  // Road (black shoulders, white interior, lane dashes)
  drawRoad();

  // Traffic cars
  for (int i = 0; i < NUM_TRAFFIC; i++) {
    if (!traffic[i].active) continue;
    drawTrafficCar(LANE_CX[traffic[i].lane], (int)traffic[i].y);
  }

  // Nitro flame
  if (nitroFrames > 0) drawNitroFlame(playerX, PLAYER_Y);

  // Player car
  drawPlayerCar(playerX, PLAYER_Y);

  // ── HUD ───────────────────────────────────────────
  // Score (top-left in white text area... but shoulders are black)
  // Draw score in road area top-right
  display.setTextColor(BLACK);
  display.setCursor(ROAD_L + 1, 0);
  display.print(score);

  // Speed indicator: small filled bar on right shoulder
  // (currentSpeed mapped to 0-5 height)
  int speedH = map((int)(currentSpeed * 10), 0, 80, 0, SCR_H - 2);
  speedH = constrain(speedH, 0, SCR_H - 2);
  display.fillRect(SCR_W - 4, SCR_H - 1 - speedH, 3, speedH, WHITE);

  // Nitro pips (bottom-left shoulder, white circles)
  for (int i = 0; i < MAX_NITRO; i++) {
    int py2 = SCR_H - 4 - i * 5;
    if (i < nitroCharges)
      display.fillRect(2, py2, 5, 4, WHITE);   // full pip
    else
      display.drawRect(2, py2, 5, 4, WHITE);   // empty pip
  }
  // "N" label above pips
  display.setTextColor(WHITE);
  display.setCursor(2, 0);
  display.print("N");
  display.setTextColor(BLACK);

  // Brake indicator: "BRK" flashes on shoulder when braking
  if (heldBrake) {
    display.setTextColor(WHITE);
    display.setCursor(ROAD_R + 1, 20);
    display.print("B");
    display.setCursor(ROAD_R + 1, 28);
    display.print("R");
    display.setCursor(ROAD_R + 1, 36);
    display.print("K");
    display.setTextColor(BLACK);
  }

  display.display();
  delay(25); // ~40fps
}
