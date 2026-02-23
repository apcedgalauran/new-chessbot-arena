import serial
import threading
import time
import logging

log = logging.getLogger("chessbot")

class SerialBridge:
    def __init__(self, port="/dev/ttyACM0", baud=9600):
        self.port = port
        self.baud = baud
        self.ser = None
        self.game_manager = None
        self.running = False

    def set_game_manager(self, gm):
        self.game_manager = gm

    def start(self):
        self.running = True
        threading.Thread(target=self._loop, daemon=True).start()

    def send(self, msg):
        if self.ser and self.ser.is_open:
            try:
                self.ser.write((msg + "\n").encode())
                log.info(f"[TX] {msg}")
            except Exception as e:
                log.warning(f"Serial write failed: {e}")

    def _loop(self):
        # Connection retry loop
        while self.running:
            if self.ser is None:
                try:
                    self.ser = serial.Serial(self.port, self.baud, timeout=1)
                    log.info(f"Connected to {self.port}")
                    time.sleep(2) # Arduino reset delay
                except Exception:
                    time.sleep(2)
                    continue

            try:
                if not self.ser.is_open:
                    self.ser = None
                    continue

                line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                if line:
                    log.info(f"[RX] {line}")
                    self._process_message(line)
            except Exception as e:
                log.error(f"Serial error: {e}")
                self.ser = None
                time.sleep(1)

    def _process_message(self, line):
        if not self.game_manager: return

        # Delegate to Game Manager
        if line.startswith("START:"):
            parts = line[6:].split(",")
            if len(parts) == 4:
                self.game_manager.reset_game(
                    difficulty=int(parts[0]),
                    timer_mins=int(parts[1]),
                    increment_secs=int(parts[2]),
                    player_color="white" if parts[3] == "W" else "black"
                )
        elif line.startswith("MOVE:"):
            self.game_manager.process_move(line[5:])
        elif line == "HINT":
            self.game_manager.request_hint()
        elif line == "REQ_HISTORY":
            self.game_manager.send_history()
        elif line == "RESIGN":
            self.game_manager.resign()
        elif line.startswith("DEPTH:"):
            try:
                self.game_manager.set_difficulty(int(line[6:]))
            except: pass
        elif line.startswith("TIME:"):
            # Sync time from Arduino if needed
            pass