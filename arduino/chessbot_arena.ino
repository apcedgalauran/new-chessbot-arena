#include <LiquidCrystal_I2C.h>

// ============================================================
// FORWARD DECLARATIONS (required for Renesas/UNO R4 architecture)
// ============================================================
enum State {
  MAIN_MENU,
  PLAYER_INPUT,
  THINKING,
  EVAL_ANIM,
  MOVE_DISPLAY,
  ILLEGAL,
  PROMOTION,
  SETTINGS,
  SETTINGS_CONFIRM,
  MOVE_HISTORY,
  GAME_END,
  PLAY_CONFIG,
  TUTORIAL
};

void transitionTo(State next);
void handleKey(char key);
void handleMainMenuKey(char key);
void handleInputKey(char key);
void handleMoveDisplayKey(char key);
void handleSettingsKey(char key);
void handleSettingsConfirmKey(char key);
void handleMoveHistoryKey(char key);
void handleGameEndKey(char key);
void handlePlayConfigKey(char key);
void handleTutorialKey(char key);
void handlePromotionKey(char key);
void handleSerial();
void handleTimer();
void processSerialMessage(String msg);
void drawMainMenu();
void drawSettings();
void drawMoveHistory();
void drawGameEnd();
void drawPlayConfig();
void drawTutorial();
void drawTimerRow();
void setupKeypad();
char scanKeypad();
char mapKey(char k);
void runThinkingAnimation();
void runEvalAnimation();
void runMoveDisplay();
void runIllegalTimeout();
void applyTimerSetting();
void applyIncrementSetting();
void applyIncrementAndSwitch();
void addToHistory(String move);
void startGame();
void triggerTimeout(String winner);
String formatTime(unsigned long secs);
String settingsRowText(int index);
String configRowText(int index);
int getEvalAnimIndex(String label);
void setRGB(int r, int g, int b);
void playTone(int freq, int duration);
void playKeyClick();
void playMoveSound();
void playErrorSound();
void triggerEvalEffects(String label);
void playVictorySound();

// ============================================================
// HARDWARE CONFIG — HARDCODED KEYPAD SCAN (no library)
// ============================================================
const byte ROW_PINS[4] = {12, 8, 7, 6};
const byte COL_PINS[4] = {5, 4, 3, 2};

char keyMap[4][4] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

char lastKey         = 0;
char lastRawKey      = 0;
char lastFiredKey    = 0;
unsigned long lastDebounce  = 0;
unsigned long lastFireTime  = 0;
const unsigned long DEBOUNCE_MS   = 30;
const unsigned long FIRE_COOLDOWN = 250; // min ms before same key can fire again

void setupKeypad() {
  for (int i = 0; i < 4; i++) {
    pinMode(ROW_PINS[i], OUTPUT);
    digitalWrite(ROW_PINS[i], HIGH);
  }
  for (int i = 0; i < 4; i++) {
    pinMode(COL_PINS[i], INPUT_PULLUP);
  }
}

char scanKeypad() {
  char detected = 0;
  for (int r = 0; r < 4; r++) {
    digitalWrite(ROW_PINS[r], LOW);
    for (int c = 0; c < 4; c++) {
      if (digitalRead(COL_PINS[c]) == LOW) {
        detected = keyMap[r][c];
      }
    }
    digitalWrite(ROW_PINS[r], HIGH);
  }

  // Track raw change for debounce timer
  if (detected != lastRawKey) {
    lastRawKey   = detected;
    lastDebounce = millis();
  }

  // No key held — reset fired tracker
  if (detected == 0) {
    lastFiredKey = 0;
    return 0;
  }

  // Wait for debounce to settle
  if ((millis() - lastDebounce) < DEBOUNCE_MS) return 0;

  // Different key from last fired — fire immediately
  if (detected != lastFiredKey) {
    lastFiredKey = detected;
    lastFireTime = millis();
    return detected;
  }

  // Same key still held — only re-fire after cooldown (handles missed releases)
  if ((millis() - lastFireTime) > FIRE_COOLDOWN) {
    lastFireTime = millis();
    return detected;
  }

  return 0;
}
LiquidCrystal_I2C lcd(0x27, 16, 2);

#define PIN_RED   9
#define PIN_BLUE  10
#define PIN_GREEN 11
#define PIN_BUZZ  13

// ============================================================
// STATE MACHINE
// ============================================================

State currentState    = PLAYER_INPUT;
State preSettingsState = PLAYER_INPUT;
State tutorialReturnState = MAIN_MENU;
int   tutorialPage        = 0;

// ============================================================
// TIMER
// ============================================================
unsigned long timerWhite   = 600;
unsigned long timerBlack   = 600;
unsigned long incrementSecs = 0;
bool timerEnabled  = true;
bool timerPaused   = false;
bool isWhiteTurn   = true;
unsigned long lastTick = 0;

// Timer option tables
const int TIMER_COUNT = 7;
int timerOptions[TIMER_COUNT]     = {0, 1, 3, 5, 10, 15, 30};
int timerOptionIndex              = 4; // default 10 min

const int INCREMENT_COUNT = 8;
int incrementOptions[INCREMENT_COUNT] = {0, 1, 2, 3, 5, 10, 15, 30};
int incrementOptionIndex              = 0; // default Off

// ============================================================
// GAME CONFIG
// ============================================================
int  difficulty  = 5;
bool playAsWhite = true;

// ============================================================
// INPUT
// ============================================================
String inputBuffer = "";
String evalLabel   = "";
String lastMoveStr = "None";
String pendingMove = "";       // Store input until confirmed valid
unsigned long lastInputActivity = 0;
bool   hashToggled = false;

// ============================================================
// MOVE HISTORY
// ============================================================
const int MAX_HISTORY = 40;
String moveHistory[MAX_HISTORY];
int historyCount  = 0;
int historyScroll = 0;

// ============================================================
// SERIAL RECEIVE BUFFER
// ============================================================
String serialBuffer = "";

// ============================================================
// ANIMATION — THINKING (non-blocking)
// ============================================================
const char* dotFrames[] = {
  ".               ",
  "..              ",
  "...             ",
  "                "
};
int dotFrame = 0;
unsigned long lastDotUpdate = 0;
const unsigned long DOT_INTERVAL = 400;

// ============================================================
// ANIMATION — EVAL (non-blocking)
// ============================================================
struct EvalAnim { const char* frames[4]; };

EvalAnim anims[] = {
  // BRILLIANT (0-3)
  { { "(*^v^*)", "(*^o^*)", "(*^v^*)", "(*^o^*)    " } }, // Cheering
  { { "\\(>v<)/", "/(>v<)\\", "\\(>v<)/", "/(>v<)\\   " } }, // Waving
  { { "(O_O)  ", "(*_*)  ", "(O_O)  ", "(*_*)      " } }, // Realization
  { { "(!_!)  ", "(^o^)  ", "(!_!)  ", "(^o^)      " } }, // Excitement
  // GOOD (4-7)
  { { "(^_^)  ", "( ^_^)",  "(^_^)  ", "( ^_^)     " } }, // Swaying
  { { "(o_o)  ", "(-_-)  ", "(o_o)  ", "(-_-)      " } }, // Nodding
  { { "(^.^)  ", "(^o^)  ", "(^.^)  ", "(^o^)      " } }, // Talking
  { { "(o_o)  ", "(O_O)  ", "(o_o)  ", "(O_O)      " } }, // Interest
  // INAC (8-11)
  { { "(O_O ) ", "(_ _ ) ", "(O_O ) ", "(_ _ )     " } }, // The "Look & Bow" you asked for
  { { "(o_O)  ", "(O_o)  ", "(o_O)  ", "(O_o)      " } }, // Confused tilt
  { { "(-_-)  ", "(._.)  ", "(-_-)  ", "(._.)      " } }, // Disappointed
  { { "(>_<)  ", "( >_<)",  "(>_<)  ", "( >_<)     " } }, // Cringe
  // MISTAKE (12-15)
  { { "(T_T)  ", "( T_T)",  "(T_T)  ", "( T_T)     " } }, // Crying shake
  { { "(;_;)  ", "( ;_;)",  "(;_;)  ", "( ;_;)     " } }, // Weeping
  { { "(._.)  ", "(,_,)  ", "(._.)  ", "(,_,)      " } }, // Shame
  { { "(>_<)  ", "(x_x)  ", "(>_<)  ", "(x_x)      " } }, // Ouch
  // BLUNDER (16-19)
  { { "(X_X)  ", "(x_x)  ", "(X_X)  ", "(x_x)      " } }, // Dead
  { { "(O_O)  ", "(>_<)  ", "(O_O)  ", "(>_<)      " } }, // Panic
  { { "(@_@)  ", "(@.@)  ", "(@_@)  ", "(@.@)      " } }, // Dizzy
  { { "(ToT)  ", "(ToT)  ", "(ToT)  ", "(ToT)      " } }  // Wailing
};

int  evalAnimIndex  = 1;
int  evalFrame      = 0;
unsigned long lastEvalUpdate = 0;
const unsigned long EVAL_FRAME_INTERVAL = 600;

// ============================================================
// MOVE DISPLAY — timer pause
// ============================================================
unsigned long moveDisplayStart = 0;
const unsigned long MOVE_DISPLAY_PAUSE = 3000;

// ============================================================
// ILLEGAL — auto return
// ============================================================
unsigned long illegalStart = 0;
const unsigned long ILLEGAL_DURATION = 2000;

// ============================================================
// SETTINGS
// ============================================================
const int SETTINGS_COUNT = 5;
int settingsScroll  = 0;

// ============================================================
// GAME END
// ============================================================
String gameEndMsg    = "";
int    gameEndCursor = 0; // 0=Play, 1=Analysis

// ============================================================
// PLAY CONFIG
// ============================================================
const int CONFIG_COUNT = 5;
int configScroll = 0;

// ============================================================
// PROMOTION
// ============================================================
String promotionBase = "";

// ============================================================
// DEBUG — Virtual LCD mirror + key/state logging
// ============================================================
String lcdRow0 = "                ";
String lcdRow1 = "                ";

void debugLCD() {
  Serial.println(F("[LCD]+-----------------+"));
  Serial.print(F("[LCD]| "));
  Serial.print(lcdRow0);
  Serial.println(F(" |"));
  Serial.print(F("[LCD]| "));
  Serial.print(lcdRow1);
  Serial.println(F(" |"));
  Serial.println(F("[LCD]+-----------------+"));
}

void debugKey(char key) {
  Serial.print(F("[KEY] Received: '"));
  Serial.print(key);
  Serial.print(F("'  State: "));
  Serial.println(stateLabel(currentState));
}

void debugState(State next) {
  Serial.print(F("[STATE] "));
  Serial.print(stateLabel(currentState));
  Serial.print(F(" -> "));
  Serial.println(stateLabel(next));
}

String stateLabel(State s) {
  switch (s) {
    case MAIN_MENU:        return "MAIN_MENU";
    case PLAYER_INPUT:     return "PLAYER_INPUT";
    case THINKING:         return "THINKING";
    case EVAL_ANIM:        return "EVAL_ANIM";
    case MOVE_DISPLAY:     return "MOVE_DISPLAY";
    case ILLEGAL:          return "ILLEGAL";
    case PROMOTION:        return "PROMOTION";
    case SETTINGS:         return "SETTINGS";
    case SETTINGS_CONFIRM: return "SETTINGS_CONFIRM";
    case MOVE_HISTORY:     return "MOVE_HISTORY";
    case GAME_END:         return "GAME_END";
    case PLAY_CONFIG:      return "PLAY_CONFIG";
    case TUTORIAL:         return "TUTORIAL";
    default:               return "UNKNOWN";
  }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  setupKeypad();
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  setRGB(50, 50, 50); // Default dim white
  randomSeed(analogRead(0));
  Serial.begin(9600);
  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("ChessBot Arena  ");
  lcd.setCursor(0, 1);
  lcd.print("  Initializing  ");
  delay(2000);
  lcd.clear();

  applyTimerSetting();
  applyIncrementSetting();
  tutorialReturnState = MAIN_MENU;
  transitionTo(TUTORIAL);
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  handleSerial();
  handleTimer();

  char key = scanKeypad();
  if (key) handleKey(key);

  switch (currentState) {
    case THINKING:     runThinkingAnimation(); break;
    case EVAL_ANIM:    runEvalAnimation();     break;
    case MOVE_DISPLAY: runMoveDisplay();       break;
    case ILLEGAL:      runIllegalTimeout();    break;
    case PLAYER_INPUT:
      // Handle 3-second timeout to revert to "Last: ..." if input is empty
      if (inputBuffer.length() == 0) {
        if (millis() - lastInputActivity > 3000) {
          String r0 = "Last: " + lastMoveStr + "                ";
          lcdWrite(0, 0, r0.substring(0, 16));
        } else {
          String row = "Move:           ";
          if (hashToggled) row.setCharAt(15, '^');
          lcdWrite(0, 0, row);
        }
      }
      break;
    default: break;
  }
}

// ============================================================
// KEY DISPATCH
// ============================================================
void handleKey(char key) {
  debugKey(key);
  playKeyClick();
  // * opens/closes settings from any gameplay state
  if (key == '*') {
    if (currentState == SETTINGS ||
        currentState == SETTINGS_CONFIRM ||
        currentState == MOVE_HISTORY) {
      transitionTo(preSettingsState);
      return;
    }
    if (currentState != GAME_END && currentState != PLAY_CONFIG) {
      preSettingsState = currentState;
      settingsScroll = 0;
      transitionTo(SETTINGS);
      return;
    }
  }

  switch (currentState) {
    case MAIN_MENU:          handleMainMenuKey(key);       break;
    case PLAYER_INPUT:       handleInputKey(key);          break;
    case MOVE_DISPLAY:       handleMoveDisplayKey(key);    break;
    case SETTINGS:           handleSettingsKey(key);       break;
    case SETTINGS_CONFIRM:   handleSettingsConfirmKey(key);break;
    case MOVE_HISTORY:       handleMoveHistoryKey(key);    break;
    case GAME_END:           handleGameEndKey(key);        break;
    case PLAY_CONFIG:        handlePlayConfigKey(key);     break;
    case PROMOTION:          handlePromotionKey(key);      break;
    case TUTORIAL:           handleTutorialKey(key);       break;
    default: break;
  }
}

// ============================================================
// MAIN MENU
// ============================================================
const int MENU_COUNT = 1; // expand later
int menuScroll = 0;

void handleMainMenuKey(char key) {
  if (key == 'A') {
    if (menuScroll > 0) { menuScroll--; drawMainMenu(); }
  } else if (key == 'B') {
    if (menuScroll < MENU_COUNT - 1) { menuScroll++; drawMainMenu(); }
  } else if (key == 'C' || key == '0') {
    if (menuScroll == 0) {
      configScroll = 0;
      transitionTo(PLAY_CONFIG);
    }
  }
}

void drawMainMenu() {
  switch (menuScroll) {
    case 0: lcdWrite(0, 0, "> New Game      "); break;
    default: break;
  }
  lcdWrite(0, 1, "C:Select        ");
}

// ============================================================
// PLAYER INPUT
// ============================================================
void handleInputKey(char key) {
  lastInputActivity = millis();

  if (key == '#') {
    hashToggled = !hashToggled;
    // Fall through to redraw
  } else if (key == '9') {
    inputBuffer = "";
    hashToggled = false;
    // Clear typing row and restore timer
    drawTimerRow();
    return;
  } else if (key == '0') {
    // Allow submitting if length >= 2 (e.g. "e4", "g5", "e2e4")
    if (inputBuffer.length() >= 2) {
      Serial.print("MOVE:");
      Serial.println(inputBuffer);
      pendingMove = inputBuffer; // Store tentatively, don't update lastMoveStr yet
      hashToggled = false;
      transitionTo(THINKING);
    }
    return;
  } else {
    // Max length 5 (e.g. "a7a8q")
    if (inputBuffer.length() >= 5) return;

    char mapped = mapKey(key);
    if (mapped != 0) {
      inputBuffer += mapped;
    }
  }

  // Unified display update
  String row = "Move: ";
  row += inputBuffer;
  while (row.length() < 15) row += " ";
  row += (hashToggled ? "^" : " ");   // position 15
  lcdWrite(0, 0, row.substring(0, 16));
}

char mapKey(char k) {
  if (k >= '1' && k <= '8') return k;
  if (!hashToggled) {
    if (k == 'A') return 'a';
    if (k == 'B') return 'b';
    if (k == 'C') return 'c';
    if (k == 'D') return 'd';
  } else {
    if (k == 'A') return 'e';
    if (k == 'B') return 'f';
    if (k == 'C') return 'g';
    if (k == 'D') return 'h';
  }
  return 0;
}

// ============================================================
// MOVE DISPLAY KEY
// ============================================================
void handleMoveDisplayKey(char key) {
  // Player can press 0 to skip the 3-second pause early
  if (key == '0') {
    applyIncrementAndSwitch();
    transitionTo(PLAYER_INPUT);
  }
}

// ============================================================
// SETTINGS
// ============================================================
void handleSettingsKey(char key) {
  if (key == 'A') {
    if (settingsScroll > 0) { settingsScroll--; drawSettings(); }
  } else if (key == 'B') {
    if (settingsScroll < SETTINGS_COUNT - 1) { settingsScroll++; drawSettings(); }
  } else if (key == 'C') {
    switch (settingsScroll) {
      case 0:
        historyScroll = 0;
        transitionTo(MOVE_HISTORY);
        break;
      case 1:
        Serial.println("HINT");
        transitionTo(preSettingsState);
        break;
      case 2:
        difficulty = (difficulty % 10) + 1;
        Serial.print("DEPTH:");
        Serial.println(difficulty);
        drawSettings();
        break;
      case 3:
        tutorialReturnState = SETTINGS;
        transitionTo(TUTORIAL);
        break;
      case 4:
        transitionTo(SETTINGS_CONFIRM);
        break;
    }
  } else if (key == 'D') {
    transitionTo(preSettingsState);
  }
}

void drawSettings() {
  String row = ">" + settingsRowText(settingsScroll);
  while (row.length() < 16) row += " ";
  lcdWrite(0, 0, row.substring(0, 16));
  lcdWrite(0, 1, "C:OK      D:Back");
}

String settingsRowText(int index) {
  switch (index) {
    case 0: return "View History";
    case 1: return "Hint/Best Move";
    case 2: return "Diff:" + String(difficulty);
    case 3: return "Controls Info";
    case 4: return "Resign";
    default: return "";
  }
}

// ============================================================
// SETTINGS CONFIRM
// ============================================================
void handleSettingsConfirmKey(char key) {
  if (key == 'C') {
    gameEndMsg    = playAsWhite ? "WHITE Resigned  " : "BLACK Resigned  ";
    Serial.println("RESIGN");
    gameEndCursor = 0;
    transitionTo(GAME_END);
  } else if (key == 'D') {
    transitionTo(SETTINGS);
  }
}

// ============================================================
// MOVE HISTORY
// ============================================================
void handleMoveHistoryKey(char key) {
  if (key == 'A' && historyScroll > 0) {
    historyScroll -= 2;
    if (historyScroll < 0) historyScroll = 0;
    drawMoveHistory();
  } else if (key == 'B' && historyScroll + 2 < historyCount) {
    historyScroll += 2;
    drawMoveHistory();
  } else if (key == 'D') {
    transitionTo(SETTINGS);
  }
}

void drawMoveHistory() {
  if (historyCount == 0) {
    lcdWrite(0, 0, "No moves yet.   ");
  } else {
    int moveNum = historyScroll / 2 + 1;
    String w = (historyScroll < historyCount)     ? moveHistory[historyScroll]     : "----";
    String b = (historyScroll + 1 < historyCount) ? moveHistory[historyScroll + 1] : "----";
    char buf[17];
    snprintf(buf, sizeof(buf), "%2d.%-5s %-5s", moveNum, w.c_str(), b.c_str());
    lcdWrite(0, 0, String(buf));
  }
  lcdWrite(0, 1, "A:Up B:Dn D:Back");
}

void addToHistory(String move) {
  if (historyCount < MAX_HISTORY) {
    moveHistory[historyCount++] = move;
  }
}

// ============================================================
// GAME END
// ============================================================
void handleGameEndKey(char key) {
  if (key == 'C') {
    transitionTo(MAIN_MENU);
  }
}

void drawGameEnd() {
  String msg = gameEndMsg;
  while (msg.length() < 16) msg += " ";
  lcdWrite(0, 0, msg.substring(0, 16));
  lcdWrite(0, 1, "C: Play Again   ");
  debugLCD();
}

// ============================================================
// PLAY CONFIG
// ============================================================
void handlePlayConfigKey(char key) {
  if (key == 'A') {
    if (configScroll > 0) { configScroll--; drawPlayConfig(); }
  } else if (key == 'B') {
    if (configScroll < CONFIG_COUNT - 1) { configScroll++; drawPlayConfig(); }
  } else if (key == 'C') {
    switch (configScroll) {
      case 0:
        timerOptionIndex = (timerOptionIndex + 1) % TIMER_COUNT;
        applyTimerSetting();
        break;
      case 1:
        incrementOptionIndex = (incrementOptionIndex + 1) % INCREMENT_COUNT;
        applyIncrementSetting();
        break;
      case 2:
        playAsWhite = !playAsWhite;
        break;
      case 3:
        difficulty = (difficulty % 10) + 1;
        Serial.print("DEPTH:");
        Serial.println(difficulty);
        break;
      case 4:
        startGame();
        return;
    }
    drawPlayConfig();
  } else if (key == 'D') {
    transitionTo(MAIN_MENU);
  }
}

void drawPlayConfig() {
  String row = ">" + configRowText(configScroll);
  while (row.length() < 16) row += " ";
  lcdWrite(0, 0, row.substring(0, 16));
  if (configScroll == 4) {
    lcdWrite(0, 1, "C:Start  D:Back ");
  } else {
    lcdWrite(0, 1, "A/B:Nav C:Change");
  }
}

String configRowText(int index) {
  switch (index) {
    case 0: {
      int t = timerOptions[timerOptionIndex];
      return "Timer:" + (t == 0 ? String("Off") : String(t) + "m");
    }
    case 1: {
      int inc = incrementOptions[incrementOptionIndex];
      return "Incr:" + (inc == 0 ? String("Off") : "+" + String(inc) + "s");
    }
    case 2: return String("PlayAs:") + (playAsWhite ? "WHITE" : "BLACK");
    case 3: return "Diff:" + String(difficulty);
    case 4: return ">>> Start Game";
    default: return "";
  }
}

void startGame() {
  historyCount  = 0;
  historyScroll = 0;
  inputBuffer   = "";
  lastMoveStr   = "None";
  hashToggled   = false;
  isWhiteTurn   = playAsWhite;

  applyTimerSetting();
  applyIncrementSetting();

  // Notify Ruby of game parameters
  Serial.print("START:");
  Serial.print(difficulty);
  Serial.print(",");
  Serial.print(timerOptions[timerOptionIndex]);
  Serial.print(",");
  Serial.print(incrementOptions[incrementOptionIndex]);
  Serial.print(",");
  Serial.println(playAsWhite ? "W" : "B");

  transitionTo(PLAYER_INPUT);
}

// ============================================================
// TUTORIAL
// ============================================================
void handleTutorialKey(char key) {
  if (key == 'C') {
    tutorialPage++;
    if (tutorialPage > 4) {
      transitionTo(tutorialReturnState);
    } else {
      drawTutorial();
    }
  } else if (key == 'D') {
    transitionTo(tutorialReturnState);
  }
}

void drawTutorial() {
  String r0, r1;
  switch(tutorialPage) {
    case 0: r0="Controls Guide  "; r1="C:Next  D:Skip  "; break;
    case 1: r0="Nav: A=Up B=Dn  "; r1="C=Ok D=Back     "; break;
    case 2: r0="Input: A-D=a-d  "; r1="#=Toggle e-h    "; break;
    case 3: r0="Input: 0=Submit "; r1="9=Clear *=Menu  "; break;
    case 4: r0="Tutorial Done   "; r1="C:Continue      "; break;
    default: r0="                "; r1="                "; break;
  }
  lcdWrite(0, 0, r0);
  lcdWrite(0, 1, r1);
}

// ============================================================
// PROMOTION
// ============================================================
void handlePromotionKey(char key) {
  char piece = 0;
  if (key == '1') piece = 'b';
  if (key == '2') piece = 'n';
  if (key == '3') piece = 'r';
  if (key == '4') piece = 'q';
  if (piece != 0) {
    String fullMove = promotionBase + piece;
    Serial.print("MOVE:");
    Serial.println(fullMove);
    transitionTo(THINKING);
  }
}

// ============================================================
// SERIAL HANDLER
// ============================================================
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialBuffer.trim();
      processSerialMessage(serialBuffer);
      serialBuffer = "";
    } else {
      serialBuffer += c;
    }
  }
}

void processSerialMessage(String msg) {
  if (msg.startsWith("EVAL:")) {
    // EVAL received = Player move was valid
    addToHistory(lastMoveStr);
    evalLabel     = msg.substring(5);
    evalAnimIndex = getEvalAnimIndex(evalLabel);
    evalFrame     = 0;
    triggerEvalEffects(evalLabel);
    lastEvalUpdate = millis();
    transitionTo(EVAL_ANIM);

  } else if (msg.startsWith("LAST:")) {
    lastMoveStr = msg.substring(5);

  } else if (msg.startsWith("BEST:")) {
    String bestMove = msg.substring(5);
    // Keep original case for SAN (e.g. Nf3)
    addToHistory(bestMove);
    lastMoveStr      = bestMove;
    timerPaused      = true;
    moveDisplayStart = millis();
    String row = "Best: " + bestMove;
    while (row.length() < 16) row += " ";
    lcdWrite(0, 0, row.substring(0, 16));
    playMoveSound();
    transitionTo(MOVE_DISPLAY);

  } else if (msg == "ILLEGAL") {
    illegalStart = millis();
    setRGB(255, 0, 0);
    playErrorSound();
    // Do NOT update lastMoveStr
    transitionTo(ILLEGAL);

  } else if (msg == "PROMOTE") {
    // Ruby detected promotion required for the move just submitted
    promotionBase = inputBuffer;
    transitionTo(PROMOTION);

  } else if (msg.startsWith("CHECKMATE:")) {
    String winner = msg.substring(10);
    gameEndMsg    = winner + " Wins!      ";
    while (gameEndMsg.length() < 16) gameEndMsg += " ";
    gameEndCursor = 0;
    playVictorySound();
    transitionTo(GAME_END);

  } else if (msg == "STALEMATE") {
    gameEndMsg    = "Stalemate!      ";
    gameEndCursor = 0;
    transitionTo(GAME_END);

  } else if (msg.startsWith("HINT:")) {
    String hintMove = msg.substring(5);
    String row = "Hint: " + hintMove;
    while (row.length() < 16) row += " ";
    lcdWrite(0, 0, row.substring(0, 16));
    debugLCD();
    delay(2000);
    transitionTo(preSettingsState);

  } else if (msg.startsWith("START:")) {
    // START:diff,timer,inc,color
    int p1 = msg.indexOf(',');
    int p2 = msg.indexOf(',', p1 + 1);
    int p3 = msg.indexOf(',', p2 + 1);
    if (p1 != -1 && p2 != -1 && p3 != -1) {
      difficulty = msg.substring(6, p1).toInt();
      int t = msg.substring(p1 + 1, p2).toInt();
      incrementSecs = msg.substring(p2 + 1, p3).toInt();
      String c = msg.substring(p3 + 1);
      playAsWhite = (c == "W");
      
      timerEnabled = (t > 0);
      timerWhite = t * 60UL;
      timerBlack = t * 60UL;
      
      historyCount = 0;
      historyScroll = 0;
      inputBuffer = "";
      lastMoveStr = "None";
      pendingMove = "";
      hashToggled = false;
      isWhiteTurn = true; // White always starts
      
      transitionTo(PLAYER_INPUT);
    }
  } else if (msg == "RESIGN") {
    gameEndMsg = playAsWhite ? "WHITE Resigned  " : "BLACK Resigned  ";
    gameEndCursor = 0;
    transitionTo(GAME_END);
  } else if (msg == "DRAW") {
    gameEndMsg = "Draw Agreed     ";
    gameEndCursor = 0;
    transitionTo(GAME_END);
  } else if (msg.startsWith("DEPTH:")) {
    difficulty = msg.substring(6).toInt();
  } else if (msg.startsWith("TIME:")) {
    // TIME:white_secs,black_secs
    int p = msg.indexOf(',');
    if (p != -1) {
      timerWhite = msg.substring(5, p).toInt();
      timerBlack = msg.substring(p + 1).toInt();
      // Force redraw if currently visible
      if (currentState != SETTINGS && 
          currentState != GAME_END && 
          currentState != TUTORIAL) drawTimerRow();
    }
  }
}

int getEvalAnimIndex(String label) {
  int base = 4; // Default GOOD
  if (label == "BRILLIANT") base = 0;
  else if (label == "GOOD") base = 4;
  else if (label == "INAC") base = 8;
  else if (label == "MISTAKE") base = 12;
  else if (label == "BLUNDER") base = 16;
  
  return base + random(0, 4);
}

void setRGB(int r, int g, int b) {
  analogWrite(PIN_RED, r);
  analogWrite(PIN_GREEN, g);
  analogWrite(PIN_BLUE, b);
}

void playTone(int freq, int duration) {
  tone(PIN_BUZZ, freq, duration);
}

void playKeyClick() {
  tone(PIN_BUZZ, 1000, 10);
}

void playMoveSound() {
  // Quick ascending notification: "Ding-ding"
  tone(PIN_BUZZ, 660, 80); // E5
  delay(80);
  tone(PIN_BUZZ, 880, 150); // A5
}

void playErrorSound() {
  // Low double buzz
  tone(PIN_BUZZ, 200, 100);
  delay(120);
  tone(PIN_BUZZ, 150, 250);
}

void triggerEvalEffects(String label) {
  if (label == "BRILLIANT") {
    setRGB(0, 255, 255); // Cyan
    tone(PIN_BUZZ, 1200, 150); // Single high blip
  } else if (label == "GOOD") {
    setRGB(0, 255, 0);   // Green
    tone(PIN_BUZZ, 1000, 100); // Short blip
  } else if (label == "INAC") {
    setRGB(255, 255, 0); // Yellow
    // No sound
  } else if (label == "MISTAKE") {
    setRGB(255, 100, 0); // Orange
    tone(PIN_BUZZ, 200, 200); // Low blip
  } else if (label == "BLUNDER") {
    setRGB(255, 0, 0);   // Red
    tone(PIN_BUZZ, 100, 400); // Long low buzz
  } else {
    setRGB(50, 50, 50);
  }
}

void playVictorySound() {
  setRGB(255, 255, 255); // White flash
  // No melody, just a single success tone
  tone(PIN_BUZZ, 1000, 400);
}

// ============================================================
// TIMER
// ============================================================
void handleTimer() {
  if (!timerEnabled || timerPaused) return;
  if (currentState == MAIN_MENU ||
      currentState == GAME_END  ||
      currentState == PLAY_CONFIG ||
      currentState == TUTORIAL) return;
  if (millis() - lastTick < 1000) return;

  lastTick = millis();

  if (isWhiteTurn) {
    if (timerWhite > 0) timerWhite--;
    if (timerWhite == 0) { triggerTimeout("Black"); return; }
  } else {
    if (timerBlack > 0) timerBlack--;
    if (timerBlack == 0) { triggerTimeout("White"); return; }
  }

  // Redraw timer row only when it's visible
  if (currentState != SETTINGS &&
      currentState != SETTINGS_CONFIRM &&
      currentState != MOVE_HISTORY &&
      currentState != GAME_END) {
    // Always draw timer on Row 1 during gameplay, even if typing on Row 0
    drawTimerRow();
  }
}

void triggerTimeout(String winner) {
  gameEndMsg    = winner + " Wins! Time ";
  while (gameEndMsg.length() < 16) gameEndMsg += " ";
  gameEndCursor = 0;
  playTone(200, 1000); // Time out buzz
  transitionTo(GAME_END);
}

void drawTimerRow() {
  char buf[17];
  snprintf(buf, sizeof(buf), "W%s  B%s",
    formatTime(timerWhite).c_str(),
    formatTime(timerBlack).c_str());
  lcdWrite(0, 1, String(buf));
}

String formatTime(unsigned long secs) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", secs / 60, secs % 60);
  return String(buf);
}

void applyTimerSetting() {
  unsigned long mins = timerOptions[timerOptionIndex];
  timerEnabled = (mins > 0);
  timerWhite   = mins * 60;
  timerBlack   = mins * 60;
}

void applyIncrementSetting() {
  incrementSecs = incrementOptions[incrementOptionIndex];
}

void applyIncrementAndSwitch() {
  if (timerEnabled && incrementSecs > 0) {
    if (isWhiteTurn) timerWhite += incrementSecs;
    else             timerBlack += incrementSecs;
  }
  timerPaused = false;
  isWhiteTurn = !isWhiteTurn;
}

// ============================================================
// ANIMATIONS — NON-BLOCKING
// ============================================================
void runThinkingAnimation() {
  if (millis() - lastDotUpdate < DOT_INTERVAL) return;
  lastDotUpdate = millis();
  dotFrame = (dotFrame + 1) % 4;
  lcdWrite(0, 0, String(dotFrames[dotFrame]));
}

void runEvalAnimation() {
  if (millis() - lastEvalUpdate < EVAL_FRAME_INTERVAL) return;
  lastEvalUpdate = millis();

  String frame = String(anims[evalAnimIndex].frames[evalFrame]);
  while (frame.length() < 16) frame += " ";
  lcdWrite(0, 0, frame.substring(0, 16));

  evalFrame++;
  if (evalFrame >= 4) {
    dotFrame = 0;
    lastDotUpdate = millis();
    transitionTo(THINKING);
  }
}

void runMoveDisplay() {
  if (millis() - moveDisplayStart < MOVE_DISPLAY_PAUSE) return;
  applyIncrementAndSwitch();
  transitionTo(PLAYER_INPUT);
}

void runIllegalTimeout() {
  if (millis() - illegalStart < ILLEGAL_DURATION) return;
  transitionTo(PLAYER_INPUT);
}

// ============================================================
// TRANSITION
// ============================================================

// LCD mirror helper — write to physical LCD and debug buffer simultaneously
void lcdWrite(int col, int row, String text) {
  lcd.setCursor(col, row);
  lcd.print(text);
  String &target = (row == 0) ? lcdRow0 : lcdRow1;
  for (int i = 0; i < (int)text.length() && (col + i) < 16; i++) {
    target.setCharAt(col + i, text[i]);
  }
}

void transitionTo(State next) {
  debugState(next);
  currentState = next;

  switch (next) {
    case MAIN_MENU:
      menuScroll = 0;
      lcd.clear();
      lcdRow0 = "                ";
      lcdRow1 = "                ";
      drawMainMenu();
      break;

    case PLAYER_INPUT:
      inputBuffer = "";
      hashToggled = false;
      lastInputActivity = 0; // Force "Last:" display immediately
      {
        String r0 = "Last: " + lastMoveStr + "                ";
        lcdWrite(0, 0, r0.substring(0, 16));
        setRGB(50, 50, 50); // Dim white for input
      }
      drawTimerRow();
      break;

    case THINKING:
      dotFrame = 0;
      lastDotUpdate = millis();
      lcd.setCursor(0, 0);
      lcd.print("                ");
      setRGB(0, 0, 128); // Blue for thinking
      break;

    case EVAL_ANIM:
      // Row 1 updated by runEvalAnimation()
      break;

    case MOVE_DISPLAY:
      // Row 1 already written by processSerialMessage before transitioning
      break;

    case ILLEGAL:
      lcdWrite(0, 0, "Illegal Move!   ");
      break;

    case PROMOTION:
      lcdWrite(0, 0, "1B 2N 3R 4Q     ");
      break;

    case SETTINGS:
      drawSettings();
      break;

    case SETTINGS_CONFIRM:
      lcdWrite(0, 0, "Resign?         ");
      lcdWrite(0, 1, "C:Yes     D:No  ");
      break;

    case MOVE_HISTORY:
      drawMoveHistory();
      break;

    case GAME_END:
      timerPaused = true;
      drawGameEnd();
      break;

    case PLAY_CONFIG:
      drawPlayConfig();
      break;

    case TUTORIAL:
      tutorialPage = 0;
      drawTutorial();
      break;
  }

  debugLCD();
}
