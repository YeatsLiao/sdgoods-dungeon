"""日志探针：后台抓 COM6 日志 N 秒，期间主进程发注入字符，落盘 monitor_log.txt。

用途：定位「注入到底有没有进引擎」（看 gesture/viewport tap/dg.game 三条链）。
注意：本脚本与 play_test.py 一样，一次运行只能开一次串口 —— 打开 / 关闭 COM6
会拉 DTR/RTS 复位 ESP32-S3，跨会话注入必然丢状态。
"""
import serial, time, sys, threading

port = "COM6"
chars = sys.argv[1] if len(sys.argv) > 1 else ""
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

buf = bytearray()
stop = threading.Event()

def reader(s):
    while not stop.is_set():
        try:
            data = s.read(256)
            if data:
                buf.extend(data)
        except Exception:
            break

s = serial.Serial(port, 115200, timeout=0.2)
th = threading.Thread(target=reader, args=(s,), daemon=True)
th.start()
time.sleep(1.0)
for c in chars:
    s.write(bytes([ord(c)]))
    s.flush()
    time.sleep(1.0)
time.sleep(duration)
stop.set()
time.sleep(0.5)
s.close()
open("monitor_log.txt", "wb").write(bytes(buf))
print(f"captured {len(buf)} bytes")
