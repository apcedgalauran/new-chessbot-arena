import sqlite3
import logging

DB_PATH = "chessbot.db"
log = logging.getLogger("chessbot")

class DatabaseManager:
    def __init__(self):
        self.init_db()

    def get_connection(self):
        conn = sqlite3.connect(DB_PATH)
        conn.row_factory = sqlite3.Row
        return conn

    def init_db(self):
        with self.get_connection() as conn:
            # Games table (No user_id anymore)
            conn.execute('''CREATE TABLE IF NOT EXISTS games (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                white_player TEXT,
                black_player TEXT,
                result TEXT,
                pgn TEXT,
                date TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )''')
            conn.commit()

    def save_game(self, white, black, result, pgn):
        try:
            with self.get_connection() as conn:
                conn.execute(
                    "INSERT INTO games (white_player, black_player, result, pgn) VALUES (?, ?, ?, ?)",
                    (white, black, result, pgn)
                )
                conn.commit()
        except Exception as e:
            log.error(f"DB Error saving game: {e}")

    def get_recent_games(self, limit=50):
        with self.get_connection() as conn:
            return conn.execute("SELECT * FROM games ORDER BY date DESC LIMIT ?", (limit,)).fetchall()