#!/usr/bin/env python3
"""
l2bot: minimal C4 game-server client used to test l2auth end to end.

It skips the login server: it writes data/accounts/<user>/access.txt itself
(same format login_server.c writes) and then speaks the game protocol.

usage:
  python3 tests/l2bot.py smoke            # functional checks (one/two bots)
  python3 tests/l2bot.py load N SECONDS   # N bots, half in a blob, half spread over the map
  python3 tests/l2bot.py siege N SECONDS  # N bots all in the same blob (mass pvp)
"""
import asyncio, os, random, struct, sys, time, math, collections

HOST, PORT = "127.0.0.1", 7777
BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bin")
KEY0 = bytes([0x94, 0x35, 0x00, 0x00, 0xa1, 0x6c, 0x54, 0x87])

OPS = {0x00: "KeyPacket", 0x01: "MoveToLocation", 0x03: "CharInfo", 0x04: "UserInfo",
       0x05: "Attack", 0x06: "Die", 0x0e: "StatusUpdate", 0x12: "DeleteObject",
       0x13: "CharSelectInfo", 0x15: "CharSelected", 0x16: "NpcInfo", 0x19: "CharCreateOk",
       0x1b: "ItemList?", 0x25: "ActionFailed", 0x28: "TeleportToLocation",
       0x2a: "TargetUnselected", 0x2b: "AutoAttackStart", 0x2c: "AutoAttackStop",
       0x47: "StopMove", 0x48: "MagicSkillUse", 0x58: "SkillList", 0x60: "MoveToPawn",
       0x61: "ValidateLocation", 0x64: "SystemMessage", 0x76: "MagicSkillLaunched",
       0x7e: "LeaveWorld", 0x80: "QuestList", 0xa6: "MyTargetSelected"}


class Cipher:
    def __init__(self):
        self.k = bytearray(KEY0)

    def _advance(self, n):
        old = int.from_bytes(self.k[0:4], "little")
        old = (old + n) & 0xffffffff
        self.k[0:4] = old.to_bytes(4, "little")

    def encrypt(self, body: bytes) -> bytes:
        out = bytearray(body); temp = 0
        for i in range(len(out)):
            out[i] = (out[i] ^ self.k[i & 7] ^ temp) & 0xff
            temp = out[i]
        self._advance(len(out))
        return bytes(out)

    def decrypt(self, body: bytes) -> bytes:
        out = bytearray(body); temp = 0
        for i in range(len(out)):
            t2 = out[i]
            out[i] = (t2 ^ self.k[i & 7] ^ temp) & 0xff
            temp = t2
        self._advance(len(out))
        return bytes(out)


def S(s):  # utf-16le null-terminated
    return s.encode("utf-16le") + b"\0\0"


def write_access(user):
    d = os.path.join(BIN, "data", "accounts", user)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "access.txt"), "w") as f:
        f.write("login_ok1=11\nlogin_ok2=22\ncreated_at=%d\nvalid_until=%d\n"
                % (time.time(), time.time() + 3600))


class Bot:
    def __init__(self, user, race=0, cls=0):
        self.user, self.race, self.cls = user, race, cls
        self.enc, self.dec = Cipher(), Cipher()
        self.encrypted = False
        self.bytes_in = 0
        self.packets = collections.Counter()
        self.inbox = asyncio.Queue()
        self.obj_id = None
        self.x = self.y = self.z = 0
        self.skills = []
        self.known = set()       # object ids the client was told about
        self.movers = set()      # distinct objects this client got move/attack/cast packets for
        self.log = []

    async def connect(self):
        self.r, self.w = await asyncio.open_connection(HOST, PORT)
        self.reader_task = asyncio.create_task(self._read_loop())

    async def _read_loop(self):
        try:
            while True:
                hdr = await self.r.readexactly(2)
                size = struct.unpack("<H", hdr)[0]
                body = await self.r.readexactly(size - 2)
                self.bytes_in += size
                if self.encrypted:
                    body = self.dec.decrypt(body)
                op = body[0]
                self.packets[op] += 1
                if op in (0x01, 0x60, 0x05, 0x48) and len(body) >= 5:
                    self.movers.add(struct.unpack_from("<I", body, 1)[0])
                self._observe(op, body)
                await self.inbox.put((op, body))
        except (asyncio.IncompleteReadError, OSError):
            await self.inbox.put((None, b""))

    def _observe(self, op, b):
        if op == 0x04 and len(b) >= 21:
            self.x, self.y, self.z, _, self.obj_id = struct.unpack_from("<iiiiI", b, 1)
        elif op in (0x16, 0x03) and len(b) >= 5:
            oid = struct.unpack_from("<I", b, 1)[0] if op == 0x16 else struct.unpack_from("<I", b, 17)[0]
            self.known.add(oid)
        elif op == 0x12 and len(b) >= 5:
            self.known.discard(struct.unpack_from("<I", b, 1)[0])
        elif op == 0x58 and len(b) >= 5:
            n = struct.unpack_from("<I", b, 1)[0]
            off, self.skills = 5, []
            for _ in range(n):
                passive, lvl, sid = struct.unpack_from("<III", b, off)
                self.skills.append(sid); off += 12
                # optional trailing 'disabled' byte (interlude layout) is not used in c4.
        elif op in (0x28, 0x61) and len(b) >= 17:
            oid, x, y, z = struct.unpack_from("<Iiii", b, 1)
            if oid == self.obj_id:
                self.x, self.y, self.z = x, y, z

    def send(self, body: bytes):
        if self.encrypted:
            body = self.enc.encrypt(body)
        self.w.write(struct.pack("<H", len(body) + 2) + body)

    async def expect(self, op, timeout=3.0):
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("waiting for %s" % OPS.get(op, hex(op)))
            got, body = await asyncio.wait_for(self.inbox.get(), left)
            if got is None:
                raise ConnectionError("server closed connection")
            if got == op:
                return body

    async def drain(self, seconds):
        end = time.time() + seconds
        seen = []
        while time.time() < end:
            try:
                got, body = await asyncio.wait_for(self.inbox.get(), max(0.01, end - time.time()))
                if got is None:
                    raise ConnectionError("server closed connection")
                seen.append((got, body))
            except asyncio.TimeoutError:
                pass
        return seen

    async def login_and_enter(self, name=None):
        write_access(self.user)
        await self.connect()
        self.send(b"\x00" + struct.pack("<I", 656))
        await self.expect(0x00)
        self.encrypted = True
        self.send(b"\x08" + S(self.user) + struct.pack("<IIII", 0, 0, 11, 22))
        await self.expect(0x13)
        name = name or self.user
        self.send(b"\x0b" + S(name) + struct.pack("<III", self.race, 0, self.cls)
                  + b"\0" * 24 + struct.pack("<III", 0, 0, 0))
        await self.expect(0x19)
        await self.expect(0x13)
        self.send(b"\x0d" + struct.pack("<I", 0))
        await self.expect(0x15)
        self.send(b"\x03")
        await self.expect(0x04)
        self.send(b"\x3f")  # RequestSkillList

    # client -> server actions
    def move(self, tx, ty, tz, keyboard=False):
        self.send(b"\x01" + struct.pack("<iiiiiiI", tx, ty, tz, self.x, self.y, self.z, 0 if keyboard else 1))

    TELEPORT = 0x7e57  # only honored by test builds (bash build.sh test)

    def teleport(self, x, y, z):
        self.x, self.y, self.z = x, y, z
        self.validate(x, y, z, self.TELEPORT)

    def validate(self, x, y, z, heading=0):
        self.send(b"\x48" + struct.pack("<iiiiI", x, y, z, heading, 0))

    def action(self, oid, shift=0):
        self.send(b"\x04" + struct.pack("<Iiii", oid, self.x, self.y, self.z) + bytes([shift]))

    def use_skill(self, sid, ctrl=0, shift=0):
        self.send(b"\x2f" + struct.pack("<II", sid, ctrl) + bytes([shift]))

    def spawn_orc(self):
        self.send(b"\xcd")


async def smoke():
    ok = True
    def check(cond, msg):
        nonlocal ok
        print(("  PASS " if cond else "  FAIL ") + msg)
        ok &= bool(cond)

    # 1. dagger class: shadow step behind an orc.
    a = Bot("smokedagger%d" % random.randint(0, 99999), race=0, cls=7)   # human rogue
    await a.login_and_enter()
    await a.drain(0.3)
    check(a.obj_id is not None, "enter world, got UserInfo")
    check(821 in a.skills, "dagger class has Shadow Step (821) in SkillList: %s" % a.skills)
    a.spawn_orc()
    body = await a.expect(0x16)
    orc = struct.unpack_from("<I", body, 1)[0]
    ox, oy, oz = struct.unpack_from("<iii", body, 13)
    # walk away from the orc so shadow step must cover distance.
    a.teleport(a.x - 300, a.y, a.z)
    stood_x, stood_y = a.x, a.y
    a.action(orc)
    await a.expect(0xa6)
    a.use_skill(821)
    msu = await a.expect(0x48)
    check(struct.unpack_from("<I", msu, 9)[0] == 821, "MagicSkillUse for Shadow Step broadcast")
    while True:
        tp = await a.expect(0x61)
        oid, tx, ty, tz = struct.unpack_from("<Iiii", tp, 1)
        if oid == a.obj_id:
            break
    d = math.hypot(tx - ox, ty - oy)
    check(20 <= d <= 80, "shadow step landed next to the target (dist %.1f)" % d)
    # orc faces the player that spawned it, so 'behind the orc' is on the far side.
    check(math.hypot(tx - stood_x, ty - stood_y) > 250, "landed on the far side (behind), not where we stood")
    a.use_skill(30)   # backstab
    seen = await a.drain(1.5)
    ops = [o for o, _ in seen]
    if 0x48 not in ops:
        print("    debug: got", [OPS.get(o, hex(o)) for o in ops])
    check(0x48 in ops, "Backstab cast after shadow step")
    hp_updates = [b for o, b in seen if o == 0x0e]
    check(len(hp_updates) > 0, "target HP updated by backstab")

    # 2. dwarf: combat kit + stun.
    b = Bot("smokedwarf%d" % random.randint(0, 99999), race=4, cls=53)
    await b.login_and_enter()
    await b.drain(0.3)
    for sid, nm in ((3, "Power Strike"), (100, "Stun Attack"), (260, "Hammer Crush"), (245, "Wild Sweep")):
        check(sid in b.skills, "dwarf has %s (%d)" % (nm, sid))
    b.spawn_orc()
    body = await b.expect(0x16)
    orc2 = struct.unpack_from("<I", body, 1)[0]
    b.action(orc2)
    await b.expect(0xa6)
    b.use_skill(260)
    seen = await b.drain(2.0)
    check(any(o == 0x48 and struct.unpack_from("<I", x, 9)[0] == 260 for o, x in seen), "Hammer Crush cast")
    check(any(o == 0x0e for o, _ in seen), "Hammer Crush did damage")

    # 3. keyboard (wasd) movement: server accepts and echoes to self.
    b.move(b.x + 400, b.y, b.z, keyboard=True)
    m = await b.expect(0x01)
    check(struct.unpack_from("<I", m, 1)[0] == b.obj_id, "keyboard move echoed to self")

    # 4. interest management: a far bot must not receive the near bot's moves.
    far = Bot("smokefar%d" % random.randint(0, 99999))
    await far.login_and_enter()
    await far.drain(0.3)
    far.teleport(far.x + 20000, far.y, far.z)
    await far.drain(0.5)
    before = far.packets[0x01]
    b.move(b.x - 300, b.y + 100, b.z)
    await far.drain(0.6)
    check(far.packets[0x01] == before, "far player gets no MoveToLocation from out-of-range player")

    for x in (a, b, far):
        x.w.close()
    return ok


async def load(n, seconds, siege=False):
    bots = [Bot("load%04d_%d" % (i, random.randint(0, 9999)), race=random.choice([0, 4]),
                cls=random.choice([0, 7, 53])) for i in range(n)]
    for bot in bots:
        await bot.login_and_enter()
    await asyncio.sleep(0.5)
    # half of the bots spread far away (other zones), half packed in a siege blob.
    for i, bot in enumerate(bots):
        if i % 2 and not siege:
            bot.teleport(bot.x + 8000 * (1 + i % 7), bot.y + 8000 * (i % 5), bot.z)
        else:
            bot.teleport(bot.x + random.randint(-400, 400), bot.y + random.randint(-400, 400), bot.z)
    await asyncio.sleep(0.5)
    for bot in bots:
        bot.bytes_in = 0; bot.packets.clear(); bot.movers.clear()
    t0 = time.time()

    async def walk(bot):
        while time.time() - t0 < seconds:
            ang = random.random() * 6.283
            bot.move(int(bot.x + 300 * math.cos(ang)), int(bot.y + 300 * math.sin(ang)), bot.z,
                     keyboard=random.random() < 0.5)
            bot.moves = getattr(bot, "moves", 0) + 1
            await asyncio.sleep(0.25 + random.random() * 0.25)
            while not bot.inbox.empty():
                bot.inbox.get_nowait()
    await asyncio.gather(*(walk(b) for b in bots))
    el = time.time() - t0
    near = [b for i, b in enumerate(bots) if i % 2 == 0]
    kb = sum(b.bytes_in for b in bots) / len(bots) / el / 1024
    pk = sum(sum(b.packets.values()) for b in bots) / len(bots) / el
    kbn = sum(b.bytes_in for b in near) / len(near) / el / 1024
    pkn = sum(sum(b.packets.values()) for b in near) / len(near) / el
    movers = sum(len(b.movers) for b in near) / len(near)
    churn = sum(b.packets[0x12] for b in near) / len(near) / el
    print("bots=%d secs=%.1f  avg/bot: %.1f KB/s %.0f pkts/s   blob bot: %.1f KB/s %.0f pkts/s, %.0f distinct objects animated, %.1f despawns/s"
          % (n, el, kb, pk, kbn, pkn, movers, churn))
    for b in bots:
        b.w.close()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "smoke"
    if mode == "fuzz":
        sys.exit(0 if asyncio.run(fuzz()) else 1)
    if mode == "smoke":
        sys.exit(0 if asyncio.run(smoke()) else 1)
    else:
        asyncio.run(load(int(sys.argv[2]), float(sys.argv[3]), siege=(mode == "siege")))


async def fuzz(rounds=200):
    """garbage packets before/after entering the world: the server must survive."""
    for r in range(rounds):
        b = Bot("fuzz%05d" % r)
        try:
            if r % 2:
                await b.login_and_enter()
            else:
                await b.connect()
                b.send(b"\x00" + struct.pack("<I", 656))
                b.encrypted = True
            for _ in range(random.randint(1, 20)):
                op = random.choice([0x01, 0x04, 0x0a, 0x2f, 0x36, 0x37, 0x3f, 0x48, 0x03, 0x0d, 0xcd, random.randint(0, 255)])
                body = bytes([op]) + os.urandom(random.randint(0, 64))
                b.send(body)
            if r % 7 == 0:   # raw junk incl. size 0 / huge sizes
                b.w.write(os.urandom(random.randint(1, 3000)))
            await b.w.drain()
            await asyncio.sleep(0.01)
        except (ConnectionError, TimeoutError, OSError, asyncio.TimeoutError):
            pass
        try:
            b.w.close()
        except Exception:
            pass
    # the server must still accept and serve a normal player.
    ok = Bot("fuzzcheck%d" % random.randint(0, 99999))
    await ok.login_and_enter()
    print("  PASS server alive after %d fuzz connections" % rounds)
    return True


if __name__ == "__main__":
    main()
