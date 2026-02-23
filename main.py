import logging
from flask import Flask, render_template, jsonify, request
from lib.database import DatabaseManager
from lib.serial_bridge import SerialBridge
from lib.game_manager import GameManager

# Config
logging.basicConfig(level=logging.INFO)
log = logging.getLogger("chessbot")
app = Flask(__name__)

# Initialize Components
db = DatabaseManager()
serial = SerialBridge()
game = GameManager(db, serial)
serial.set_game_manager(game)

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/history", methods=["GET"])
def api_history():
    games = db.get_recent_games()
    return jsonify({"games": [dict(g) for g in games]})

@app.route("/api/state", methods=["GET"])
def api_state():
    return jsonify(game.get_state())

@app.route("/api/move", methods=["POST"])
def api_move():
    data = request.get_json(force=True, silent=True)
    if not data: return jsonify({"error": "Invalid JSON"}), 400
    result = game.process_move(data.get("move", ""))
    return jsonify({"result": result})

@app.route("/api/start", methods=["POST"])
def api_start():
    data = request.get_json(force=True, silent=True)
    if not data: return jsonify({"error": "Invalid JSON"}), 400
    
    game.reset_game(
        difficulty=data.get("difficulty", 5),
        timer_mins=data.get("timer", 10),
        increment_secs=data.get("increment", 0),
        player_color=data.get("player_color", "white")
    )
    
    # Notify Arduino
    c = "W" if data.get("player_color") == "white" else "B"
    serial.send(f"START:{data.get('difficulty')},{data.get('timer')},{data.get('increment')},{c}")
    return jsonify({"result": "ok"})

@app.route("/api/hint", methods=["POST"])
def api_hint():
    game.request_hint()
    return jsonify({"result": "ok"})

@app.route("/api/resign", methods=["POST"])
def api_resign():
    game.resign()
    return jsonify({"result": "ok"})

if __name__ == "__main__":
    serial.start()
    log.info("ChessBot Arena starting on http://localhost:5000")
    app.run(host="0.0.0.0", port=5000, debug=False)
