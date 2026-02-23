"""
ChessBot Arena — Flask Web Application + Serial Bridge
Extends the original serial bridge between Arduino Uno R4 WiFi and Stockfish.
The serial bridge runs in a background thread. Flask serves the web dashboard.
Both share game state via a thread-safe in-memory dict.
"""

import threading
import time
import json
import logging

from flask import Flask, render_template, jsonify, request

import chess
import chess.engine

# Try importing serial; allow graceful failure if not installed or no device
try:
    import serial
except ImportError:
    serial = None

# ============================================================
# CONFIGURATION
# ============================================================
PORT = "/dev/ttyACM0"       # Serial port for Arduino
BAUD = 9600
SF_PATH = "/usr/bin/stockfish"  # Path to Stockfish binary

# On Windows, you might use something like:
# PORT = "COM3"
# SF_PATH = "stockfish.exe"

# ============================================================
# FLASK APP
# ============================================================
app = Flask(__name__)
logging.basicConfig(level=logging.INFO)
log = logging.getLogger("chessbot")

# ============================================================
# SHARED GAME STATE
# ============================================================
state_lock = threading.Lock()
game_counter = 0

state = {
    "board_fen": chess.STARTING_FEN,
    "turn": "white",
    "eval_label": None,
    "eval_cp": 0,
    "best_move": None,
    "last_move": None,
    "hint_move": None,
    "status": "waiting",        # waiting | playing | checkmate | stalemate | resigned | draw
    "winner": None,
    "move_history": [],          # list of { move, eval_label, cp_before, cp_after }
    "timer_white": 600,
    "timer_black": 600,
    "increment": 0,
    "difficulty": 5,
    "player_color": "white",
    "game_id": 0,
    "lcd_row0": "ChessBot Arena  ",
    "lcd_row1": "  Ready...      ",
    "thinking": False,
}

# The actual chess board object (shared, guarded by state_lock)
board = chess.Board()
DEPTH = 5

# Serial port handle (may be None)
ser = None

# Stockfish engine handle (may be None)
engine = None

# ============================================================
# HELPERS
# ============================================================

def classify(delta):
    """Classify move quality based on centipawn loss."""
    if delta < 0:
        return "BRILLIANT"
    if delta <= 20:
        return "GOOD"
    if delta <= 50:
        return "INAC"
    if delta <= 100:
        return "MISTAKE"
    return "BLUNDER"


def get_cp(b, depth):
    """Get centipawn evaluation of position from white's perspective."""
    if engine is None:
        return 0
    info = engine.analyse(b, chess.engine.Limit(depth=depth))
    score = info["score"].white()
    if score.is_mate():
        return 10000 if score.mate() > 0 else -10000
    return score.score()


def serial_send(msg):
    """Send a message to Arduino via serial, if connected."""
    if ser is not None:
        try:
            ser.write((msg + "\n").encode())
            ser.flush()
            log.info(f"[SERIAL TX] {msg}")
        except Exception as e:
            log.warning(f"Serial write failed: {e}")


def update_lcd(row0=None, row1=None):
    """Update LCD mirror strings in state."""
    if row0 is not None:
        state["lcd_row0"] = (row0 + "                ")[:16]
    if row1 is not None:
        state["lcd_row1"] = (row1 + "                ")[:16]


def format_timer(secs):
    """Format seconds as MM:SS."""
    m = int(secs) // 60
    s = int(secs) % 60
    return f"{m:02d}:{s:02d}"


def timer_lcd_row():
    """Build the timer LCD row string."""
    return f"W{format_timer(state['timer_white'])}  B{format_timer(state['timer_black'])}"


def process_move(move_uci):
    """
    Process a player move through the engine pipeline.
    This is the core logic shared by both serial input and web API.
    Must be called while state_lock is held.
    """
    global board, DEPTH

    move_str = move_uci.strip().lower()

    # Check for promotion: if the move is 4 chars and targets rank 1 or 8 with a pawn
    if len(move_str) == 4:
        from_sq = chess.parse_square(move_str[:2])
        to_sq = chess.parse_square(move_str[2:4])
        piece = board.piece_at(from_sq)
        if piece and piece.piece_type == chess.PAWN:
            to_rank = chess.square_rank(to_sq)
            if (piece.color == chess.WHITE and to_rank == 7) or \
               (piece.color == chess.BLACK and to_rank == 0):
                # Promotion needed but no piece specified
                serial_send("PROMOTE")
                update_lcd("1B 2N 3R 4Q     ")
                return "promote"

    # Validate move
    try:
        move = chess.Move.from_uci(move_str)
    except ValueError:
        serial_send("ILLEGAL")
        update_lcd("Illegal Move!   ")
        return "illegal"

    if move not in board.legal_moves:
        serial_send("ILLEGAL")
        update_lcd("Illegal Move!   ")
        return "illegal"

    # Evaluate position before move
    cp_before = get_cp(board, DEPTH)

    # Make the player's move
    board.push(move)
    state["board_fen"] = board.fen()
    state["last_move"] = move_str
    state["turn"] = "white" if board.turn == chess.WHITE else "black"

    # Check for game end after player move
    if board.is_checkmate():
        winner = "BLACK" if board.turn == chess.WHITE else "WHITE"
        serial_send(f"CHECKMATE:{winner}")
        state["status"] = "checkmate"
        state["winner"] = winner.lower()
        update_lcd(f"{winner} Wins!      ", ">Play  Analysis ")
        # Still record history
        cp_after = get_cp(board, DEPTH)
        delta = cp_before - cp_after
        elabel = classify(abs(delta))
        state["move_history"].append({
            "move": move_str,
            "eval_label": elabel,
            "cp_before": cp_before,
            "cp_after": cp_after,
        })
        return "checkmate"

    if board.is_stalemate() or board.is_insufficient_material() or \
       board.is_fifty_moves() or board.is_repetition():
        serial_send("STALEMATE")
        state["status"] = "stalemate"
        update_lcd("Stalemate!      ", ">Play  Analysis ")
        return "stalemate"

    # Evaluate position after player move
    cp_after = get_cp(board, DEPTH)

    # Classify the player's move
    delta = cp_before - cp_after
    # If player is black, invert delta sense
    if state["player_color"] == "black":
        delta = -delta
    eval_label = classify(delta)

    serial_send(f"EVAL:{eval_label}")
    state["eval_label"] = eval_label
    state["eval_cp"] = cp_after
    state["thinking"] = True

    # Emoticon map for LCD
    emoticons = {
        "BRILLIANT": "(@_@)Brilliant!",
        "GOOD":      "(^_^) Good!    ",
        "INAC":      "(._o)Inaccuracy",
        "MISTAKE":   "(T_T) Mistake..",
        "BLUNDER":   "(X_X) Blunder!!",
    }
    update_lcd(emoticons.get(eval_label, eval_label))

    # Record move in history
    state["move_history"].append({
        "move": move_str,
        "eval_label": eval_label,
        "cp_before": cp_before,
        "cp_after": cp_after,
    })

    # Apply increment for player
    if state["increment"] > 0:
        if state["player_color"] == "white":
            state["timer_white"] += state["increment"]
        else:
            state["timer_black"] += state["increment"]

    # Engine's turn — compute best move
    update_lcd("Thinking...     ")
    if engine is not None:
        result = engine.play(board, chess.engine.Limit(depth=DEPTH))
        best = result.move
    else:
        # No engine available — pick first legal move as fallback
        best = list(board.legal_moves)[0] if board.legal_moves else None

    if best is None:
        state["thinking"] = False
        return "no_move"

    best_uci = best.uci()

    # Evaluate before engine move (for engine move classification)
    cp_before_engine = get_cp(board, DEPTH)

    board.push(best)
    state["board_fen"] = board.fen()
    state["best_move"] = best_uci
    state["last_move"] = best_uci
    state["turn"] = "white" if board.turn == chess.WHITE else "black"
    state["thinking"] = False

    serial_send(f"BEST:{best_uci.upper()}")
    update_lcd(f"Best: {best_uci.upper()}        "[:16], timer_lcd_row())

    # Check game end after engine move
    if board.is_checkmate():
        winner = "BLACK" if board.turn == chess.WHITE else "WHITE"
        serial_send(f"CHECKMATE:{winner}")
        state["status"] = "checkmate"
        state["winner"] = winner.lower()
        update_lcd(f"{winner} Wins!      ", ">Play  Analysis ")

    elif board.is_stalemate() or board.is_insufficient_material() or \
         board.is_fifty_moves() or board.is_repetition():
        serial_send("STALEMATE")
        state["status"] = "stalemate"
        update_lcd("Stalemate!      ", ">Play  Analysis ")

    # Record engine move in history
    cp_after_engine = get_cp(board, DEPTH)
    delta_engine = cp_before_engine - cp_after_engine
    engine_eval_label = classify(abs(delta_engine))
    state["move_history"].append({
        "move": best_uci,
        "eval_label": engine_eval_label,
        "cp_before": cp_before_engine,
        "cp_after": cp_after_engine,
    })
    state["eval_cp"] = cp_after_engine

    # Apply increment for engine
    if state["increment"] > 0:
        if state["player_color"] == "white":
            state["timer_black"] += state["increment"]
        else:
            state["timer_white"] += state["increment"]

    return "ok"


def reset_game(difficulty=5, timer_mins=10, increment_secs=0, player_color="white"):
    """Reset the board and state for a new game."""
    global board, DEPTH, game_counter

    board = chess.Board()
    DEPTH = difficulty
    game_counter += 1

    state["board_fen"] = chess.STARTING_FEN
    state["turn"] = "white"
    state["eval_label"] = None
    state["eval_cp"] = 0
    state["best_move"] = None
    state["last_move"] = None
    state["hint_move"] = None
    state["status"] = "playing"
    state["winner"] = None
    state["move_history"] = []
    state["timer_white"] = timer_mins * 60 if timer_mins > 0 else 0
    state["timer_black"] = timer_mins * 60 if timer_mins > 0 else 0
    state["increment"] = increment_secs
    state["difficulty"] = difficulty
    state["player_color"] = player_color
    state["game_id"] = game_counter
    state["thinking"] = False

    update_lcd("Move: ____      ", timer_lcd_row())

    # If player is black, engine moves first
    if player_color == "black" and engine is not None:
        state["thinking"] = True
        update_lcd("Thinking...     ")
        result = engine.play(board, chess.engine.Limit(depth=DEPTH))
        best = result.move
        if best:
            cp_before = get_cp(board, DEPTH)
            board.push(best)
            cp_after = get_cp(board, DEPTH)
            best_uci = best.uci()
            state["board_fen"] = board.fen()
            state["best_move"] = best_uci
            state["last_move"] = best_uci
            state["turn"] = "white" if board.turn == chess.WHITE else "black"
            state["move_history"].append({
                "move": best_uci,
                "eval_label": "GOOD",
                "cp_before": cp_before,
                "cp_after": cp_after,
            })
            serial_send(f"BEST:{best_uci.upper()}")
            update_lcd(f"Best: {best_uci.upper()}        "[:16], timer_lcd_row())
        state["thinking"] = False


# ============================================================
# SERIAL BRIDGE THREAD
# ============================================================

def serial_loop():
    """Background thread: reads serial data from Arduino and processes commands."""
    global ser, DEPTH

    # Try to open serial port
    if serial is None:
        log.warning("pyserial not available — serial bridge disabled")
        return

    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
        log.info(f"Serial connected on {PORT}")
        time.sleep(2)  # Arduino reset delay
    except Exception as e:
        log.warning(f"Could not open serial port {PORT}: {e}")
        ser = None
        return

    buf = ""
    while True:
        try:
            if ser is None or not ser.is_open:
                time.sleep(1)
                continue

            raw = ser.read(ser.in_waiting or 1)
            if not raw:
                continue

            buf += raw.decode("utf-8", errors="ignore")

            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if not line:
                    continue

                log.info(f"[SERIAL RX] {line}")

                with state_lock:
                    if line.startswith("START:"):
                        parts = line[6:].split(",")
                        if len(parts) == 4:
                            diff = int(parts[0])
                            tmins = int(parts[1])
                            inc = int(parts[2])
                            pcolor = "white" if parts[3] == "W" else "black"
                            reset_game(diff, tmins, inc, pcolor)

                    elif line.startswith("MOVE:"):
                        move_uci = line[5:]
                        if state["status"] == "playing":
                            process_move(move_uci)

                    elif line == "HINT":
                        if engine is not None and state["status"] == "playing":
                            result = engine.play(board, chess.engine.Limit(depth=DEPTH))
                            if result.move:
                                hint = result.move.uci()
                                state["hint_move"] = hint
                                serial_send(f"HINT:{hint.upper()}")
                                update_lcd(f"Hint: {hint.upper()}        "[:16])

                    elif line == "RESIGN":
                        if state["status"] == "playing":
                            winner = "black" if state["player_color"] == "white" else "white"
                            state["status"] = "resigned"
                            state["winner"] = winner
                            update_lcd(f"{state['player_color'].upper()} Resigned  ",
                                       ">Play  Analysis ")

                    elif line == "DRAW":
                        if state["status"] == "playing":
                            state["status"] = "draw"
                            update_lcd("Draw Agreed     ", ">Play  Analysis ")

                    elif line.startswith("DEPTH:"):
                        try:
                            DEPTH = int(line[6:])
                            state["difficulty"] = DEPTH
                        except ValueError:
                            pass

                    # Also relay LCD debug lines
                    elif line.startswith("[LCD]"):
                        pass  # Informational, ignore

        except Exception as e:
            log.error(f"Serial error: {e}")
            time.sleep(1)


# ============================================================
# TIMER THREAD
# ============================================================

def timer_loop():
    """Background thread: decrements active player's timer every second."""
    while True:
        time.sleep(1)
        with state_lock:
            if state["status"] != "playing":
                continue
            if state["timer_white"] == 0 and state["timer_black"] == 0:
                continue  # Timer disabled
            if state["thinking"]:
                continue  # Don't tick while engine is computing

            if state["turn"] == state["player_color"]:
                # Player's clock
                if state["player_color"] == "white":
                    if state["timer_white"] > 0:
                        state["timer_white"] -= 1
                        if state["timer_white"] == 0:
                            state["status"] = "checkmate"
                            state["winner"] = "black"
                            serial_send("CHECKMATE:BLACK")
                            update_lcd("Black Wins! Time", ">Play  Analysis ")
                else:
                    if state["timer_black"] > 0:
                        state["timer_black"] -= 1
                        if state["timer_black"] == 0:
                            state["status"] = "checkmate"
                            state["winner"] = "white"
                            serial_send("CHECKMATE:WHITE")
                            update_lcd("White Wins! Time", ">Play  Analysis ")

            # Update LCD timer row during gameplay
            if state["status"] == "playing":
                state["lcd_row1"] = timer_lcd_row()


# ============================================================
# FLASK ROUTES
# ============================================================

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/state", methods=["GET"])
def api_state():
    with state_lock:
        return jsonify(dict(state))


@app.route("/api/move", methods=["POST"])
def api_move():
    data = request.get_json(force=True)
    move_uci = data.get("move", "").strip()
    if not move_uci:
        return jsonify({"error": "No move provided"}), 400

    with state_lock:
        if state["status"] != "playing":
            return jsonify({"error": "Game not in progress"}), 400
        result = process_move(move_uci)

    return jsonify({"result": result})


@app.route("/api/start", methods=["POST"])
def api_start():
    data = request.get_json(force=True)
    diff = data.get("difficulty", 5)
    tmins = data.get("timer", 10)
    inc = data.get("increment", 0)
    pcolor = data.get("player_color", "white")

    with state_lock:
        reset_game(diff, tmins, inc, pcolor)

    # Notify Arduino if connected
    color_code = "W" if pcolor == "white" else "B"
    serial_send(f"START:{diff},{tmins},{inc},{color_code}")

    return jsonify({"result": "ok", "game_id": state["game_id"]})


@app.route("/api/hint", methods=["POST"])
def api_hint():
    with state_lock:
        if state["status"] != "playing":
            return jsonify({"error": "Game not in progress"}), 400
        if engine is not None:
            result = engine.play(board, chess.engine.Limit(depth=DEPTH))
            if result.move:
                hint = result.move.uci()
                state["hint_move"] = hint
                serial_send(f"HINT:{hint.upper()}")
                update_lcd(f"Hint: {hint.upper()}        "[:16])
                return jsonify({"hint": hint})
        return jsonify({"error": "Engine not available"}), 500


@app.route("/api/resign", methods=["POST"])
def api_resign():
    with state_lock:
        if state["status"] != "playing":
            return jsonify({"error": "Game not in progress"}), 400
        winner = "black" if state["player_color"] == "white" else "white"
        state["status"] = "resigned"
        state["winner"] = winner
        update_lcd(f"{state['player_color'].upper()} Resigned  ", ">Play  Analysis ")
    serial_send("RESIGN")
    return jsonify({"result": "ok"})


@app.route("/api/draw", methods=["POST"])
def api_draw():
    with state_lock:
        if state["status"] != "playing":
            return jsonify({"error": "Game not in progress"}), 400
        state["status"] = "draw"
        update_lcd("Draw Agreed     ", ">Play  Analysis ")
    serial_send("DRAW")
    return jsonify({"result": "ok"})


@app.route("/api/depth", methods=["POST"])
def api_depth():
    global DEPTH
    data = request.get_json(force=True)
    new_depth = data.get("depth", DEPTH)
    with state_lock:
        DEPTH = int(new_depth)
        state["difficulty"] = DEPTH
    serial_send(f"DEPTH:{DEPTH}")
    return jsonify({"result": "ok", "depth": DEPTH})


# ============================================================
# STARTUP
# ============================================================

def init_engine():
    """Try to start Stockfish engine."""
    global engine
    try:
        engine = chess.engine.SimpleEngine.popen_uci(SF_PATH)
        log.info(f"Stockfish engine started from {SF_PATH}")
    except Exception as e:
        log.warning(f"Could not start Stockfish at {SF_PATH}: {e}")
        # Try common alternative paths
        alt_paths = ["stockfish", "stockfish.exe", "/usr/games/stockfish",
                     "/usr/local/bin/stockfish", "/opt/homebrew/bin/stockfish"]
        for path in alt_paths:
            try:
                engine = chess.engine.SimpleEngine.popen_uci(path)
                log.info(f"Stockfish engine started from {path}")
                return
            except Exception:
                continue
        log.error("Stockfish not found — engine features disabled. "
                  "Install Stockfish and update SF_PATH in main.py.")
        engine = None


if __name__ == "__main__":
    init_engine()

    # Start serial bridge thread
    serial_thread = threading.Thread(target=serial_loop, daemon=True)
    serial_thread.start()

    # Start timer thread
    timer_thread = threading.Thread(target=timer_loop, daemon=True)
    timer_thread.start()

    log.info("ChessBot Arena starting on http://localhost:5000")
    app.run(host="0.0.0.0", port=5000, debug=False)
