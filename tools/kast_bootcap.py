# KAST boot log capture — open COM9, watch for boot output, print it.
import sys, time, serial

PORT = "COM9"
BAUD = 115200
WATCH_S = 20

ser = serial.Serial(PORT, BAUD, timeout=0.5)
deadline = time.time() + WATCH_S
buf = b""
# nudge: toggle DTR/RTS like a reset so we catch a fresh boot log
ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setRTS(False)
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
ser.close()
text = buf.decode("utf-8", "replace")
print("\n---SUMMARY---")
for line in text.splitlines():
    if "BUILD" in line or "rst:" in line or "panic" in line.lower() or "KAST" in line:
        print(">>", line.strip())
