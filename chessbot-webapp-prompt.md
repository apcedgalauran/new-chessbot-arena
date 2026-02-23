# ChessBot Arena — Web Application Build Prompt

## Context

You are building the web frontend and backend for ChessBot Arena, an embedded systems project. A physical Arduino Uno R4 WiFi runs a chess interface (keypad input, I2C LCD output). A Python script (`main.py`) acts as a serial bridge between the Arduino and Stockfish chess engine. Your job is to extend `main.py` into a full Flask web application and build its frontend dashboard.

---

## Existing System You Must Not Break

### `main.py` — Current Serial Bridge (already working)

```python
import serial
import chess
import chess.engine

PORT     = "/dev/ttyACM1"
BAUD     = 9600
SF_PATH  = "/usr/bin/stockfish"

ser    = serial.Serial(PORT, BAUD, timeout=1)
engine = chess.engine.SimpleEngine.popen_uci(SF_PATH)
board  = chess.Board()
DEPTH  = 5

def classify(delta):
    if delta < 0:    return "BRILLIANT"
    if delta <= 20:  return "GOOD"
    if delta <= 50:  return "INAC"
    if delta <= 100: return "MISTAKE"
    return "BLUNDER"

def get_cp(board, depth):
    info = engine.analyse(board, chess.engine.Limit(depth=depth))
    score = info["score"].white()
    if score.is_mate(): return 10000 if score.mate() > 0 else -10000
    return score.score()
```

The serial protocol between Arduino and Python is fixed:

**Arduino → Python:**
- `START:difficulty,timer_mins,increment_secs,W|B\n`
- `MOVE:e2e4\n`
- `HINT\n`
- `RESIGN\n`
- `DRAW\n`
- `DEPTH:n\n`

**Python → Arduino:**
- `EVAL:BRILLIANT|GOOD|INAC|MISTAKE|BLUNDER\n`
- `BEST:e7e5\n`
- `ILLEGAL\n`
- `PROMOTE\n`
- `CHECKMATE:WHITE|BLACK\n`
- `STALEMATE\n`
- `HINT:e2e4\n`

---

## What You Are Building

Extend `main.py` into a Flask application. The serial bridge loop runs in a **background thread**. Flask serves the web dashboard. Both share game state via a shared in-memory object. No database required.

---

## Tech Stack

- **Backend:** Python 3, Flask, python-chess, stockfish
- **Frontend:** Single HTML file with Tailwind CSS (CDN), vanilla JavaScript
- **Serial:** pyserial (already in use)
- **Real-time:** Flask-SSE or simple polling (GET /api/state every second)
- **No database.** Game state is in-memory only.

---

## Project Structure

```
chessbot-arena/
├── main.py                  # Flask app + serial bridge (extend this)
├── templates/
│   └── index.html           # Single page dashboard
├── requirements.txt
└── arduino/
    └── chessbot.ino         # Do not modify
```

**`requirements.txt`:**
```
flask
pyserial
chess
```

---

## Shared Game State

Define a single shared state object accessible by both the serial thread and Flask routes:

```python
state = {
    "board_fen": chess.STARTING_FEN,
    "turn": "white",
    "eval_label": None,
    "best_move": None,
    "last_move": None,
    "status": "waiting",   # waiting | playing | checkmate | stalemate | resigned | draw
    "winner": None,
    "move_history": [],    # list of { move, eval_label, cp_before, cp_after }
    "timer_white": 0,
    "timer_black": 0,
    "increment": 0,
    "difficulty": 5,
    "player_color": "white",
    "game_id": None        # increment on each new game
}
```

Use a `threading.Lock()` when reading or writing state.

---

## Flask Routes

### `GET /`
Serve `templates/index.html`.

### `GET /api/state`
Return full `state` dict as JSON. Frontend polls this every second.

### `POST /api/move`
Body: `{ "move": "e2e4" }`
Inject a move as if it came from the Arduino. Useful for web-only testing when Arduino is not connected. Pushes `MOVE:e2e4` through the same processing pipeline as the serial handler.

### `POST /api/start`
Body: `{ "difficulty": 5, "timer": 10, "increment": 0, "player_color": "white" }`
Resets board, updates state, sends `START:` to Arduino via serial if connected.

### `POST /api/hint`
Triggers Stockfish hint for current position. Updates state with hint move.

### `POST /api/resign`
Ends game via resignation.

### `POST /api/draw`
Ends game via draw.

### `POST /api/depth`
Body: `{ "depth": 7 }`
Updates engine depth.

---

## Serial Bridge Thread

Run in `threading.Thread(daemon=True)`. On serial read failure (Arduino not connected), log the error and continue — the web UI must still function without hardware attached.

The thread handles all incoming serial messages and updates shared state identically to the current `main.py` logic. On each processed move, append to `state["move_history"]`.

---

## Web Dashboard — `templates/index.html`

Single HTML file. Tailwind CSS via CDN. No build step.

### Layout

Two-column desktop layout, single column mobile.

**Left column:**
- Chess board rendered from FEN using Unicode pieces
- 8×8 grid, alternating light/dark squares
- Highlight last move squares
- Below board: move input field (UCI notation, e.g. `e2e4`) + Submit button
- Below that: New Game button, Hint button, Resign button, Draw button

**Right column — top half:**
- LCD Display Simulator: two rows, monospace font, 16 chars wide, dark green background, green text — visually mimics the physical LCD
  - Row 1: current eval emoticon or move status
  - Row 2: timer display `W09:47  B10:00`
- Eval bar: vertical bar showing position advantage (white on bottom, black on top), updates from `state["eval_cp"]` if available

**Right column — bottom half:**
- Move history table: columns — #, Move, Eval, Quality
- Quality shown as colored badge: Brilliant (gold), Good (green), Inaccuracy (yellow), Mistake (orange), Blunder (red)

### LCD Emoticon Display

Mirror exactly what the Arduino LCD shows. When `state["eval_label"]` updates, show the matching emoticon sequence in the LCD simulator row 1:

| Label | Display |
|---|---|
| BRILLIANT | `(@_@) Brilliant!` |
| GOOD | `(^_^) Good!` |
| INAC | `(._o) Inaccuracy` |
| MISTAKE | `(T_T) Mistake..` |
| BLUNDER | `(X_X) Blunder!!!` |

### JavaScript

- Poll `GET /api/state` every 1000ms
- On state change, update board, LCD simulator, eval bar, timers, move history
- Submit move via `POST /api/move`
- Game controls via their respective POST endpoints
- Board rendering: parse FEN, map piece characters to Unicode symbols, render 8×8 grid
- No external chess.js dependency — render from FEN string directly

### Chess Piece Unicode Map

```
K=♔ Q=♕ R=♖ B=♗ N=♘ P=♙
k=♚ q=♛ r=♜ b=♝ n=♞ p=♟
```

---

## Move Quality Centipawn Thresholds

```python
def classify(delta):
    if delta < 0:    return "BRILLIANT"
    if delta <= 20:  return "GOOD"
    if delta <= 50:  return "INAC"
    if delta <= 100: return "MISTAKE"
    return "BLUNDER"
```

`delta = cp_before - cp_after` where both are from white's perspective.

---

## Startup

```python
if __name__ == "__main__":
    serial_thread = threading.Thread(target=serial_loop, daemon=True)
    serial_thread.start()
    app.run(host="0.0.0.0", port=5000, debug=False)
```

Access at `http://localhost:5000`.

---

## Critical Requirements

1. Serial bridge must continue functioning exactly as before — Arduino hardware flow must not be disrupted
2. Web UI must work independently if Arduino is not connected (graceful serial failure handling)
3. No database — all state is in-memory
4. No external JS libraries except Tailwind CDN
5. Single `main.py` file — do not split into multiple Python files
6. Single `templates/index.html` — all CSS and JS inline or in the one file
7. Thread safety — all state reads/writes use the lock
8. The existing serial protocol is fixed and must not change
