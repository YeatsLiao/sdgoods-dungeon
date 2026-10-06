"""单会话游玩验证：开一次串口（打开串口会复位设备，所以所有按键与截屏必须在同一
会话内完成），按脚本注入按键并多次截屏，用于真机游玩效果取证。

用法：python tools/play_test.py shots/play   →  shots/play_01.jpg ... + shots/play_log.txt
"""
import os
import re
import sys
import time

import serial

PORT = "COM6"
BAUD = 115200
BEGIN_RE = re.compile(
    r"===SHOT-BEGIN\s+w=(\d+)\s+h=(\d+)\s+bpp=(\d+)\s+(?:fmt=(\d+)\s+)?swap=(\d+)\s+bytes=(\d+)==="
)
END_MARK = b"===SHOT-END==="


class Session:
    """一个串口会话：喂入字节流，分离「日志文本行」与「截屏二进制帧」。"""

    def __init__(self, port=PORT):
        self.s = serial.Serial(port, BAUD, timeout=0.1)
        try:
            self.s.dtr = True
            self.s.rts = True
        except Exception:
            pass
        self.buf = b""
        self.logs = []
        self.frames = []          # (fmt, bytes)

    def pump(self):
        """非阻塞把已到达的字节读进缓冲区并解析。"""
        while True:
            chunk = self.s.read(1024)
            if not chunk:
                break
            self.buf += chunk
        self._parse()

    def _parse(self):
        while True:
            i = self.buf.find(b"===SHOT-BEGIN")
            if i < 0:
                # 纯日志：整行收录，末尾半行留着等下一个包
                lines = self.buf.split(b"\n")
                self._log_lines(lines[:-1])
                self.buf = lines[-1]
                return
            self._log_lines(self.buf[:i].split(b"\n"))
            self.buf = self.buf[i:]
            nl = self.buf.find(b"\n")        # buf 已以 BEGIN 开头
            if nl < 0:
                return                       # 头行未收全
            m = BEGIN_RE.match(self.buf[:nl].decode("ascii", "replace"))
            if not m:
                self.buf = self.buf[nl + 1:]
                continue
            g = m.groups()
            fmt = int(g[3]) if g[3] else 0
            total = int(g[5])
            start = nl + 1
            if len(self.buf) < start + total + len(END_MARK):
                return                       # 帧未收全，等下一个包（buf 已从 BEGIN 起）
            img = self.buf[start:start + total]
            self.frames.append((fmt, img))
            self.buf = self.buf[start + total + len(END_MARK):]

    def _log_lines(self, parts):
        for ln in parts:
            txt = ln.decode("utf-8", "replace").strip()
            if txt:
                self.logs.append(txt)

    def wait(self, seconds):
        t0 = time.time()
        while time.time() - t0 < seconds:
            self.pump()
            time.sleep(0.02)

    def press(self, ch, wait=0.35):
        self.s.write(ch.encode())
        self.s.flush()
        self.wait(wait)

    def shot(self, path):
        self.frames.clear()
        self.s.write(b"s")
        self.s.flush()
        t0 = time.time()
        while time.time() - t0 < 30:
            self.pump()
            if self.frames:
                fmt, img = self.frames.pop(0)
                out = re.sub(r"\.png$", ".jpg", path) if fmt else path
                with open(out, "wb") as f:
                    f.write(img)
                print("saved", out, "%.1f KB" % (len(img) / 1024.0))
                return out
            time.sleep(0.05)
        print("shot timeout", path)
        return None


def serpentine(steps_per_leg):
    """蛇形探索：右→下→左→下→右… 每腿 N 步（碰壁就停，下一步换方向）。"""
    seq = []
    for dirs in ["d" * steps_per_leg, "x" * 2, "a" * steps_per_leg,
                 "x" * 2, "d" * steps_per_leg, "x" * 2,
                 "a" * steps_per_leg, "x" * 2, "w" * steps_per_leg]:
        seq.append(dirs)
    return "".join(seq)


def guided(prefix, coords, walk_wait=4.0):
    """引导式游玩：按给定的屏幕坐标序列逐个精确点击，每次点击后截一张图。
    用法：python tools/play_test.py tap "175,268;210,90;" shots/guided
    """
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    ses = Session()
    print("等待开机…")
    ses.wait(6)
    ses.shot(prefix + "_01.png")
    for n, xy in enumerate([c for c in coords.split(";") if c.strip()], start=2):
        print("tap", xy.strip())
        ses.s.write(("t" + xy.strip()).encode())
        ses.s.flush()
        ses.wait(walk_wait)
        ses.shot("%s_%02d.png" % (prefix, n))
    with open(os.path.join(os.path.dirname(prefix) or ".", "guided_log.txt"),
              "w", encoding="utf-8") as f:
        f.write("\n".join(ses.logs))
    ses.s.close()


DUMP_RE = re.compile(
    r"D h=(-?\d+),(-?\d+) c=(-?\d+),(-?\d+) s=(-?\d+)/(\d+) g=(\d+) d=(\d+)F "
    r"e=(-?\d+),(-?\d+)\s+i=([^ ]*)\s+m=(.*)$")


def last_dump(logs):
    """倒着找最近一条 debug 导出（固件 'v' 命令打的）。"""
    for ln in reversed(logs):
        m = DUMP_RE.search(ln)
        if m:
            g = m.groups()
            pairs = lambda s: [tuple(int(v) for v in p.split(",")) for p in s.split("|")
                               if re.match(r"^-?\d+,-?\d+$", p)]
            return {
                "hero": (int(g[0]), int(g[1])),
                "cam": (int(g[2]), int(g[3])),
                "hp": (int(g[4]), int(g[5])),
                "gold": int(g[6]),
                "depth": int(g[7]),
                "exit": (int(g[8]), int(g[9])),
                "items": pairs(g[10]),
                "mobs": pairs(g[11]),
            }
    return None


def auto(rounds=20, prefix="shots/auto", walk_wait=5.0, prefer_exit=False):
    """闭环游玩：串口 'v' 读坐标与数值 → 挑目标 → 'n'+tile 直推让引擎跑 A*
    → 定期截屏。取证目标：gold>0 与 depth>1（prefer_exit 为后者专走楼梯）。"""
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    ses = Session()
    print("等待开机…")
    ses.wait(6)
    shot_idx = 1
    ses.shot("%s_%02d.png" % (prefix, shot_idx))
    seen_items = set()
    fail_mobs = {}
    best = {"gold": 0, "depth": 1}
    prev_gold = 0
    for r in range(rounds):
        before = len(ses.logs)
        ses.press("v", 0.5)
        d = last_dump(ses.logs[before:])
        if not d:
            print("round %d: 无坐标导出" % r)
            ses.wait(1.0)
            continue
        best["gold"] = max(best["gold"], d["gold"])
        best["depth"] = max(best["depth"], d["depth"])
        if d["gold"] > prev_gold:
            print("★ 金币 %d -> %d" % (prev_gold, d["gold"]))
        prev_gold = d["gold"]
        print("round %d: h=%s hp=%d/%d gold=%d depth=%dF items=%s mobs=%s exit=%s"
              % (r, d["hero"], d["hp"][0], d["hp"][1], d["gold"], d["depth"],
                 d["items"], d["mobs"], d["exit"]))
        if d["hp"][0] <= 0:
            shot_idx += 1
            ses.shot("%s_%02d.png" % (prefix, shot_idx))
            # 死亡画面点按 = 重开（走真实 LVGL 点击路径，验证 on_overlay_click_cb）
            print("英雄阵亡（hp=%d）→ 点屏重开" % d["hp"][0])
            ses.s.write(b"t180,180;")
            ses.s.flush()
            ses.wait(1.5)
            b2 = len(ses.logs)
            ses.press("v", 0.5)
            d2 = last_dump(ses.logs[b2:])
            if d2:
                print("★ 重开验证：depth=%dF hp=%d/%d gold=%d"
                      % (d2["depth"], d2["hp"][0], d2["hp"][1], d2["gold"]))
            break
        hx, hy = d["hero"]
        target = None
        if prefer_exit and d["exit"][0] >= 0 and d["exit"] != d["hero"]:
            target = ("楼梯", d["exit"])
        if target is None:
            for xy in d["mobs"]:            # 只顺手砍紧邻的怪，不进去送死死拼
                if max(abs(xy[0] - hx), abs(xy[1] - hy)) <= 1 and fail_mobs.get(xy, 0) < 2:
                    target = ("怪", xy)
                    break
        if target is None and not prefer_exit:
            for xy in d["items"]:
                if xy not in seen_items:
                    target = ("金币", xy)
                    break
        if target is None and not prefer_exit:
            for xy in d["mobs"]:        # 路过就清掉（未黑名单的）
                if fail_mobs.get(xy, 0) < 2:
                    target = ("怪", xy)
                    break
        if target is None and d["exit"][0] >= 0 and d["exit"] != d["hero"]:
            target = ("楼梯", d["exit"])
        if target is None:
            print("round %d: 无可去目标，原地等待" % r)
            ses.press("q", 0.5)
            continue
        kind, (tx, ty) = target
        ses.s.write(("n%d,%d;" % (tx, ty)).encode())
        ses.s.flush()
        ses.wait(walk_wait)
        if kind == "金币":
            seen_items.add((tx, ty))
        if kind == "怪":
            fail_mobs[(tx, ty)] = fail_mobs.get((tx, ty), 0) + 1
        if r % 2 == 1 or kind in ("楼梯", "金币"):
            shot_idx += 1
            ses.shot("%s_%02d.png" % (prefix, shot_idx))
    print("最终取证：gold=%d depth=%dF" % (best["gold"], best["depth"]))
    shot_idx += 1
    ses.shot("%s_%02d.png" % (prefix, shot_idx))
    ses.press("j", 0.8)                        # 打开背包看 stats（金币/深度）
    shot_idx += 1
    ses.shot("%s_%02d.png" % (prefix, shot_idx))
    with open(os.path.join(os.path.dirname(prefix) or ".", "auto_log.txt"),
              "w", encoding="utf-8") as f:
        f.write("\n".join(ses.logs))
    print("log lines:", len(ses.logs))
    ses.s.close()


def main():
    if len(sys.argv) > 3 and sys.argv[1] == "tap":
        guided(sys.argv[3], sys.argv[2])
        return
    if len(sys.argv) > 1 and sys.argv[1] in ("auto", "descend"):
        rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 14
        auto(rounds=rounds, prefer_exit=(sys.argv[1] == "descend"),
             prefix="shots/descend" if sys.argv[1] == "descend" else "shots/auto")
        return
    prefix = sys.argv[1] if len(sys.argv) > 1 else "shots/play"
    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)
    ses = Session()
    print("等待开机…")
    ses.wait(6)
    ses.shot(prefix + "_01.png")

    # A* 长途寻路：轮流点视口四角（相机跟随 → 角落不断变化 → 覆盖面大）
    for corner in "oikuo":
        ses.press(corner, 0.3)
        ses.wait(3.0)                          # 给 A* 分帧走路留时间
    ses.press("e", 0.5)                        # 搜索秘密门
    ses.shot(prefix + "_02.png")

    ses.press("j", 0.8)                        # 背包 overlay
    ses.shot(prefix + "_03.png")
    ses.press("j", 0.3)                        # 关闭

    for corner in "uoi":
        ses.press(corner, 0.3)
        ses.wait(3.0)
    for ch in serpentine(10)[:20]:             # 补一段单格走，撞怪概率更高
        ses.press(ch, 0.22)
    ses.shot(prefix + "_04.png")

    for corner in "kuo":
        ses.press(corner, 0.3)
        ses.wait(3.0)
    ses.press("q", 0.5)                        # 等待（怪会靠近）
    ses.shot(prefix + "_05.png")

    with open(os.path.join(os.path.dirname(prefix) or ".", "play_log.txt"),
              "w", encoding="utf-8") as f:
        f.write("\n".join(ses.logs))
    print("log lines:", len(ses.logs))
    ses.s.close()


if __name__ == "__main__":
    main()
