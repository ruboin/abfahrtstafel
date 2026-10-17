"""Print the departure board in the terminal. Usage: python cli.py [rows]"""
import logging
import sys

import bvg_client
import config

logging.basicConfig(level=logging.WARNING, format="%(levelname)s %(message)s")

rows = int(sys.argv[1]) if len(sys.argv) > 1 else 8
board = bvg_client.get_board(max_rows=rows, include_cancelled=True)

print(f"{config.STATION_LABEL}  ({board['station_id']}, via {board['source']})  {board['updated']}"
      + ("  [STALE]" if board["stale"] else ""))
if board["error"]:
    print("Error:", board["error"])
print("-" * 46)
for d in board["departures"]:
    mins = "jetzt" if d["minutes"] == 0 else f"{d['minutes']} min"
    extra = " AUSFALL" if d["cancelled"] else (f" +{d['delay_min']}" if d["delay_min"] else "")
    print(f"{d['line']:<5} {d['direction'][:26]:<26} {mins:>7}{extra}")
if not board["departures"]:
    print("(no departures)")
