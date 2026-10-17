"""Settings for the departure board (dev server)."""

# S+U Tempelhof (Berlin) - one VBB stop covering U6 and the S-Bahn platforms
STATION_ID = "900068201"

# Primary: VBB HAFAS backend (same as the VBB app). No key needed.
HAFAS_URL = "https://fahrinfo.vbb.de/bin/mgate.exe"

# Fallback: community REST wrappers (frequently overloaded). Tried in order.
REST_BASES = [
    "https://v6.bvg.transport.rest",
    "https://v6.vbb.transport.rest",
]

# Which products to show
PRODUCTS = {
    "suburban": True,  # S-Bahn
    "subway": True,    # U-Bahn
    "tram": False,
    "bus": False,
    "ferry": False,
    "express": False,
    "regional": False,
}

# Display: GeekMagic SmallTV Pro, 1.54" 240x240 IPS (ST7789)
DISPLAY_W = 240
DISPLAY_H = 240
HEADER_H = 30            # station name + clock
MAX_ROWS = 6             # (240 - 30) / 6 = 35 px per row
DIR_MAX_CHARS = 20      # direction text that fits next to badge + minutes
STATION_LABEL = "Tempelhof"

MIN_MINUTES = 1          # hide departures you cannot reach anyway (e.g. walking time)
DURATION_MIN = 60        # look-ahead window for the API
REFRESH_SECONDS = 15     # how often clients should poll
CACHE_SECONDS = 10       # do not hit the API more often than this
STALE_MAX_SECONDS = 600  # serve old data (marked stale) up to this age on API failure
HTTP_TIMEOUT = 8
RETRIES = 2

HOST = "0.0.0.0"
PORT = 5000

# BVG / S-Bahn line colours
LINE_COLORS = {
    "U1": "#7DAD4C", "U2": "#DA421E", "U3": "#16683D", "U4": "#F0D722",
    "U5": "#7E5330", "U6": "#8C6DAB", "U7": "#528DBA", "U8": "#224F86",
    "U9": "#F3791D",
    "S1": "#DE4DA4", "S2": "#005F27", "S25": "#005F27", "S26": "#005F27",
    "S3": "#0A4C99", "S41": "#AD5937", "S42": "#CB6418", "S45": "#CD9C53",
    "S46": "#CD9C53", "S47": "#CD9C53", "S5": "#EB7405", "S7": "#816DA6",
    "S75": "#816DA6", "S8": "#66AA22", "S85": "#66AA22", "S9": "#992746",
}
DEFAULT_COLORS = {"subway": "#115D91", "suburban": "#008D4F"}
