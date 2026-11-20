"""Give the clock its Wi-Fi network (2.4 GHz) so it can get the time from NTP. Run this yourself in a terminal:
    python set_wifi.py COM9
It asks for the network name and password (the password is typed hidden), sends them to the board over USB serial,
and the board keeps them in its flash. They are not stored on this PC and are not in the firmware source.
Run it with no password (just press Enter) for an open network. Run  python set_wifi.py COM9 --forget  to clear them.
Use the ESP-IDF Python (it has pyserial)."""
import getpass, sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "COM9"
forget = "--forget" in sys.argv
if forget:
    line = b"W\n"
else:
    ssid = input("Wi-Fi network name (2.4 GHz): ").strip()
    pw = getpass.getpass("Password (hidden; Enter for none): ")
    if not ssid or "\t" in ssid + pw or "\n" in ssid + pw:
        sys.exit("Need a network name without tabs or line breaks.")
    line = ("W" + ssid + "\t" + pw + "\n").encode()

with serial.Serial() as s:
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = False          # keep the board out of reset / download mode while the port opens
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(line)
    s.flush()
    print("Sent. Waiting up to 25 s for the board to connect...")
    end = time.time() + 25
    ok = False
    while time.time() < end and not ok:
        text = s.readline().decode("utf-8", "replace").rstrip()
        for key in ("WIFI", "NTP"):                          # the board never prints the password
            if key in text:
                print("  " + text[text.index(key):])
                ok = ok or "NTP synced" in text or forget
print("Done: the board got the time from NTP." if ok and not forget else "Done." if forget else
      "No NTP sync seen yet: check the name/password and that the router offers 2.4 GHz.")
