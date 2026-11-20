"""This PC's time zone as a POSIX TZ string (what the ESP32 firmware needs), taken from Windows' zone name."""
import subprocess, time

POSIX = {
    "Pacific Standard Time": "PST8PDT,M3.2.0,M11.1.0",
    "Mountain Standard Time": "MST7MDT,M3.2.0,M11.1.0",
    "US Mountain Standard Time": "MST7",                      # Arizona: no daylight saving
    "Central Standard Time": "CST6CDT,M3.2.0,M11.1.0",
    "Eastern Standard Time": "EST5EDT,M3.2.0,M11.1.0",
    "Alaskan Standard Time": "AKST9AKDT,M3.2.0,M11.1.0",
    "Hawaiian Standard Time": "HST10",
    "UTC": "UTC0",
    "GMT Standard Time": "GMT0BST,M3.5.0/1,M10.5.0",
    "W. Europe Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Central Europe Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Romance Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Central European Standard Time": "CET-1CEST,M3.5.0,M10.5.0/3",
}

def posix_tz():
    try:
        name = subprocess.run(["tzutil", "/g"], capture_output=True, text=True).stdout.strip()
    except OSError:
        name = ""
    if name in POSIX:
        return POSIX[name], name
    off = -time.timezone                                       # fallback: fixed offset, no daylight saving
    sign = "+" if off >= 0 else "-"
    hh, mm = divmod(abs(off) // 60, 60)
    label = "%s%02d%s" % (sign, hh, "%02d" % mm if mm else "")
    return "<%s>%s%d%s" % (label, "-" if off >= 0 else "", hh, ":%02d" % mm if mm else ""), name or "unknown zone (fixed offset)"
