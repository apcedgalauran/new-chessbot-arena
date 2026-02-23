import threading
import time
import random
import chess
import chess.engine
import logging

log = logging.getLogger("chessbot")

class GameManager:
    def __init__(self, db, serial, sf_path="/usr/bin/stockfish"):
        self.db = db
        self.serial = serial
        self.lock = threading.Lock()
        self.board = chess.Board()
        self.engine = None
        self.sf_path = sf_path
        
        # Game Settings
        self.depth = 5
        self.timer_white = 600
        self.timer_black = 600
        self.increment = 0
        self.player_color = "white"
        self.game_id = 0

        # State
        self.state = {
            "status": "waiting",
            "turn": "white",
            "fen": chess.STARTING_FEN,
            "last_move": None,
            "check": False,
            "eval": {"val": 0, "label": None},
            "history": [],
            "lcd": ["ChessBot Arena", "Ready..."]
        }

        self._init_engine()
        threading.Thread(target=self._timer_loop, daemon=True).start()

    def _init_engine(self):
        try:
            self.engine = chess.engine.SimpleEngine.popen_uci(self.sf_path)
        except Exception as e:
            log.error(f"Stockfish failed: {e}")

    def get_state(self):
        with self.lock:
            # Return a snapshot
            s = self.state.copy()
            s["timers"] = {"w": self.timer_white, "b": self.timer_black}
            s["settings"] = {"depth": self.depth, "id": self.game_id}
            return s

    def reset_game(self, difficulty=5, timer_mins=10, increment_secs=0, player_color="white"):
        with self.lock:
            self.board.reset()
            self.depth = difficulty
            self.timer_white = timer_mins * 60
            self.timer_black = timer_mins * 60
            self.increment = increment_secs
            self.player_color = player_color
            self.game_id += 1
            
            self.state.update({
                "status": "playing",
                "turn": "white",
                "fen": chess.STARTING_FEN,
                "last_move": None,
                "check": False,
                "eval": {"val": 0, "label": None},
                "history": [],
                "lcd": ["Last: None", f"W{timer_mins:02}:00 B{timer_mins:02}:00"]
            })

            # If AI is white (player is black), AI moves
            if player_color == "black":
                threading.Thread(target=self._ai_move_task, args=(self.game_id,), daemon=True).start()

    def process_move(self, move_str):
        move_str = move_str.strip()
        with self.lock:
            if self.state["status"] != "playing": return "not_playing"

            # Parse Move
            move = None
            try:
                move = chess.Move.from_uci(move_str.lower())
            except:
                try:
                    move = self.board.parse_san(move_str)
                except: pass
            
            # 3. Try Destination Square
            if move is None:
                try:
                    if len(move_str) == 2:
                        target_sq = chess.parse_square(move_str.lower())
                        candidates = [m for m in self.board.legal_moves if m.to_square == target_sq]
                        if candidates:
                            def get_priority(m):
                                p = self.board.piece_at(m.from_square)
                                if not p: return 0
                                mapping = {
                                    chess.KING: 6, chess.QUEEN: 5, chess.ROOK: 4,
                                    chess.KNIGHT: 3, chess.BISHOP: 2, chess.PAWN: 1
                                }
                                return mapping.get(p.piece_type, 0)
                            candidates.sort(key=get_priority, reverse=False)
                            move = candidates[0]
                except ValueError:
                    pass
            
            if not move or move not in self.board.legal_moves:
                self.serial.send("ILLEGAL")
                self._update_lcd(row0="Illegal Move!")
                return "illegal"

            # Execute Player Move
            self._make_move(move, send_feedback=True)
            
            # Check Game Over
            if self._check_game_over(): return "game_over"

            # Trigger AI
            threading.Thread(target=self._ai_move_task, args=(self.game_id,), daemon=True).start()
            return "ok"

    def _make_move(self, move, send_feedback=True):
        # 1. Eval before move (for quality)
        cp_before = self._get_eval()
        
        # Generate SAN before pushing
        san = self.board.san(move)

        # 2. Push move
        self.board.push(move)
        
        # 3. Eval after move
        cp_after = self._get_eval()
        
        # 4. Calculate Delta & Quality
        delta = cp_before - cp_after
        # If it was black's turn, invert delta
        if self.board.turn == chess.WHITE: # It is now White's turn, so Black just moved
            delta = -delta
            
        quality = self._classify_move(delta)
        
        # 5. Update State
        self.state["fen"] = self.board.fen()
        self.state["last_move"] = move.uci()
        self.state["turn"] = "white" if self.board.turn == chess.WHITE else "black"
        self.state["check"] = self.board.is_check()
        self.state["eval"] = {"val": cp_after, "label": quality}
        self.state["history"].append({
            "move": move.uci(),
            "san": san,
            "quality": quality,
            "eval": cp_after
        })

        # 6. Update Hardware
        if send_feedback:
            self.serial.send(f"LAST:{san}")
            self.serial.send(f"EVAL:{quality}")
            self._update_lcd_emoticon(quality)
        
        # 7. Timers
        if self.increment > 0:
            if self.board.turn == chess.WHITE: # Black just moved
                self.timer_black += self.increment
            else:
                self.timer_white += self.increment
        self._sync_time()

    def _ai_move_task(self, game_id):
        time.sleep(random.uniform(2.0, 4.0)) # Thinking time
        
        with self.lock:
            if self.game_id != game_id or self.state["status"] != "playing": return
            self._update_lcd(row0="Thinking...")
            
        # Calculate
        result = self.engine.play(self.board, chess.engine.Limit(depth=self.depth))
        
        with self.lock:
            if self.game_id != game_id or self.state["status"] != "playing": return
            
            if result.move:
                san = self.board.san(result.move)
                self.serial.send(f"BEST:{san}")
                self._update_lcd(row0=f"Last: {san}")
                self._make_move(result.move, send_feedback=False)
                self._check_game_over()

    def _check_game_over(self):
        result = None
        reason = ""
        
        if self.board.is_checkmate():
            winner = "Black" if self.board.turn == chess.WHITE else "White"
            result = f"{winner} Wins"
            reason = "Checkmate"
            self.serial.send(f"CHECKMATE:{winner.upper()}")
        elif self.board.is_stalemate():
            result = "Draw"
            reason = "Stalemate"
            self.serial.send("STALEMATE")
        elif self.board.is_insufficient_material():
            result = "Draw"
            reason = "Insufficient Material"
            self.serial.send("STALEMATE")
            
        if result:
            self.state["status"] = "game_over"
            self._update_lcd(row0=f"{result}!", row1=reason[:16])
            self._save_to_db(result)
            return True
        return False

    def _save_to_db(self, result):
        pgn = " ".join([m["san"] for m in self.state["history"]])
        white = "Player" if self.player_color == "white" else "Stockfish"
        black = "Stockfish" if self.player_color == "white" else "Player"
        self.db.save_game(white, black, result, pgn)

    def _get_eval(self):
        if not self.engine: return 0
        info = self.engine.analyse(self.board, chess.engine.Limit(depth=self.depth))
        score = info["score"].white()
        if score.is_mate(): return 10000 if score.mate() > 0 else -10000
        return score.score()

    def _classify_move(self, delta):
        if delta < 0: return "BRILLIANT"
        if delta <= 30: return "GOOD"
        if delta <= 90: return "INAC"
        if delta <= 200: return "MISTAKE"
        return "BLUNDER"

    def _update_lcd(self, row0=None, row1=None):
        if row0: self.state["lcd"][0] = (row0 + "                ")[:16]
        if row1: self.state["lcd"][1] = (row1 + "                ")[:16]

    def _update_lcd_emoticon(self, label):
        emotes = {
            "BRILLIANT": ["(*^O^*) Super!! ", "\\(^o^)/ Wow!!   "],
            "GOOD":      ["(^_^) Good!     ", "(o_o) Nice move "],
            "INAC":      ["(O_O ) (_ _ )   ", "(-_-) Hmmm..... "],
            "MISTAKE":   ["(T_T) Mistake.. ", "(;_;) Oh no.... "],
            "BLUNDER":   ["(X_X) Blunder!! ", "(>_<) Disaster! "]
        }
        self._update_lcd(row0=random.choice(emotes.get(label, ["Analyzing..."])))

    def _sync_time(self):
        self.serial.send(f"TIME:{int(self.timer_white)},{int(self.timer_black)}")

    def _timer_loop(self):
        while True:
            time.sleep(1)
            with self.lock:
                if self.state["status"] != "playing": continue
                
                if self.state["turn"] == "white" and self.timer_white > 0:
                    self.timer_white -= 1
                    if self.timer_white == 0: self._timeout("Black")
                elif self.state["turn"] == "black" and self.timer_black > 0:
                    self.timer_black -= 1
                    if self.timer_black == 0: self._timeout("White")
                
                # Update LCD timer row
                w_min, w_sec = divmod(int(self.timer_white), 60)
                b_min, b_sec = divmod(int(self.timer_black), 60)
                self.state["lcd"][1] = f"W{w_min:02}:{w_sec:02} B{b_min:02}:{b_sec:02}"
                self._sync_time()

    def _timeout(self, winner):
        self.state["status"] = "game_over"
        self.serial.send(f"CHECKMATE:{winner.upper()}")
        self._update_lcd(row0=f"{winner} Wins!", row1="Time Expired")
        self._save_to_db(f"{winner} Wins (Time)")

    def request_hint(self):
        threading.Thread(target=self._hint_task, daemon=True).start()

    def _hint_task(self):
        if not self.engine: return
        with self.lock:
            if self.state["status"] != "playing": return
            board_copy = self.board.copy()
        
        try:
            result = self.engine.play(board_copy, chess.engine.Limit(depth=self.depth))
            if result.move:
                hint_san = board_copy.san(result.move)
                with self.lock:
                    self.state["hint_move"] = hint_san
                    self._update_lcd(row0=f"Hint: {hint_san}")
                self.serial.send(f"HINT:{hint_san}")
        except Exception as e:
            log.error(f"Hint error: {e}")

    def resign(self):
        with self.lock:
            if self.state["status"] != "playing": return
            winner = "Black" if self.player_color == "white" else "White"
            self.state["status"] = "resigned"
            self.state["winner"] = winner.lower()
            self._update_lcd(row0=f"{winner} Wins!", row1="Resignation")
            self.serial.send("RESIGN")
            self._save_to_db(f"{winner} Wins (Resign)")

    def draw(self):
        with self.lock:
            if self.state["status"] != "playing": return
            self.state["status"] = "draw"
            self._update_lcd(row0="Draw Agreed", row1="Game Over")
            self.serial.send("DRAW")
            self._save_to_db("Draw (Agreed)")

    def send_history(self):
        games = self.db.get_recent_games(limit=5)
        self.serial.send("HISTORY_CLEAR")
        for g in games:
            # Format result: "White Wins" -> "1-0"
            res = g["result"]
            if "White" in res and "Win" in res: r = "1-0"
            elif "Black" in res and "Win" in res: r = "0-1"
            else: r = "1/2"
            
            # Truncate names to 4 chars to fit 16x2 LCD
            w = g["white_player"][:4]
            b = g["black_player"][:4]
            
            self.serial.send(f"HISTORY_ADD:{w}-{b} {r}")

    def set_difficulty(self, d):
        self.depth = d