"""Set the clock's time zone and time from this PC over USB serial (without Wi-Fi/NTP).
Usage: python set_time.py COM9
With Wi-Fi set up (set_wifi.py) the clock gets the time from NTP and this is not needed."""
import sys, time, datetime
import serial  # pyserial (comes with the ESP-IDF Python environment)
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from tzinfo import posix_tz

port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
tz, tzname = posix_tz()
with serial.Serial() as s:
    s.port, s.baudrate = port, 115200
    s.dtr = False          # keep the board out of reset / download mode while the port opens
    s.rts = False
    s.open()
    s.write(b"Z" + tz.encode() + b"\n")
    s.flush()
    time.sleep(0.2)
    t = int(time.time()) + 1                                   # send on a whole second (UTC seconds since 1970)
    time.sleep(max(0.0, t - time.time()))
    s.write(b"T%d\n" % t)
    s.flush()
    print("sent time zone %s (%s) and %s UTC to %s" % (tz, tzname, datetime.datetime.utcfromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S"), port))
