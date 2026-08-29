#!/usr/bin/env python3
"""Drive emil.wasm under a real pty and assert on the rendered screen.

Usage: tests/wasix/pty_test.py [--sandbox DIR] [emil.wasm]

Why this exists: every other WASIX test drives the editor down a pipe.
Wasmer reports isatty(0)==1 regardless, so emil enters raw mode and
reads the pipe as keystrokes -- and with no tty driver in between,
every byte arrives.  A real terminal has one, and wasmer forwards only
ECHO and ICANON to it, so C-s, C-q and C-c are swallowed and
C-x C-s / C-x C-c stop working.  The pipe-driven suites cannot see
that.  This one can.

Run it with and without tests/wasix/emil-wasix to see the difference.

Manual only: nothing in CI or the Makefile invokes this.  The advisory
wasix-pty job drives tests/decoder_pty_test.c instead, which asserts on
the editor's behaviour rather than on the rendered screen.  This one is
for looking at a terminal problem by hand.

Deliberately not a unit test: this is front-door usage.  We allocate a
pty, set its window size, run the editor inside it, feed keystrokes at
human-ish pace, and read back what a terminal would have shown.
"""
import os, pty, re, select, subprocess, sys, time, fcntl, termios, struct

ANSI = re.compile(rb'\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\x1b[78]|\x1b\][^\x07\x1b]*(\x07|\x1b\\)')

class Term:
    """Minimal VT100 screen model: enough for CUP, EL, ED and text."""
    def __init__(self, rows=24, cols=80):
        self.rows, self.cols = rows, cols
        self.grid = [[' '] * cols for _ in range(rows)]
        self.cy = self.cx = 0

    def feed(self, data):
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0x1b:
                m = re.match(rb'\x1b\[([0-9;?]*)([A-Za-z])', data[i:])
                if m:
                    self._csi(m.group(1).decode(), m.group(2).decode())
                    i += m.end(); continue
                m = re.match(rb'\x1b\][^\x07\x1b]*(\x07|\x1b\\)', data[i:])
                if m: i += m.end(); continue
                m = re.match(rb'\x1b[()][A-Z0-9]|\x1b[78=>]', data[i:])
                if m: i += m.end(); continue
                i += 1; continue
            if b == 0x0d: self.cx = 0; i += 1; continue
            if b == 0x0a:
                self.cy = min(self.cy + 1, self.rows - 1); i += 1; continue
            if b == 0x08: self.cx = max(0, self.cx - 1); i += 1; continue
            if b < 0x20: i += 1; continue
            # decode one utf-8 char
            ln = 1
            if b >= 0xf0: ln = 4
            elif b >= 0xe0: ln = 3
            elif b >= 0xc0: ln = 2
            ch = data[i:i+ln].decode('utf-8', 'replace')
            if self.cy < self.rows and self.cx < self.cols:
                self.grid[self.cy][self.cx] = ch
            self.cx += 1
            if self.cx >= self.cols:
                self.cx = self.cols - 1
            i += ln

    def _csi(self, params, final):
        p = [int(x) for x in params.split(';') if x.isdigit()]
        if final == 'H':
            self.cy = (p[0] - 1) if len(p) > 0 else 0
            self.cx = (p[1] - 1) if len(p) > 1 else 0
            self.cy = max(0, min(self.cy, self.rows - 1))
            self.cx = max(0, min(self.cx, self.cols - 1))
        elif final == 'K':
            n = p[0] if p else 0
            if n == 0:
                for x in range(self.cx, self.cols): self.grid[self.cy][x] = ' '
        elif final == 'J':
            n = p[0] if p else 0
            if n in (0, 2):
                start = self.cy + 1 if n == 0 else 0
                for y in range(start, self.rows):
                    self.grid[y] = [' '] * self.cols

    def text(self):
        return '\n'.join(''.join(r).rstrip() for r in self.grid)

    def line(self, n):
        return ''.join(self.grid[n]).rstrip()


class Session:
    def __init__(self, argv, rows=24, cols=80, cwd=None):
        self.term = Term(rows, cols)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            if cwd: os.chdir(cwd)
            os.environ['TERM'] = 'xterm-256color'
            os.environ['LANG'] = 'C.UTF-8'
            # The terminal setup tests/wasix/emil-wasix performs, applied here
            # before exec for the same reason it does: wasmer forwards
            # only ECHO, ICANON and IEXTEN, so without this C-s is eaten
            # as XOFF and C-c raises SIGINT against wasmer.  Set
            # NO_TTY_FIX=1 to run without it and see the difference.
            #
            # ISIG is not cleared.  wasmer re-asserts it at startup, so
            # clearing it here would not survive; disabling the c_cc
            # slots leaves it with no character to act on, and wasmer
            # preserves c_cc.  That is what makes this race-free -- an
            # earlier version slept 0.8s and cleared ISIG afterwards.
            if not os.environ.get('NO_TTY_FIX'):
                a = termios.tcgetattr(0)
                a[0] &= ~(termios.ICRNL | termios.IXON | termios.IXOFF |
                          termios.BRKINT)
                a[1] &= ~termios.OPOST
                cc = a[6]
                for slot in (termios.VINTR, termios.VSUSP, termios.VQUIT,
                             termios.VSTOP, termios.VSTART):
                    cc[slot] = 0
                termios.tcsetattr(0, termios.TCSANOW, a)
            os.execvp(argv[0], argv)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack('HHHH', rows, cols, 0, 0))
        self.raw = b''

    def pump(self, seconds=0.6):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try: chunk = os.read(self.fd, 65536)
                except OSError: break
                if not chunk: break
                self.raw += chunk
                self.term.feed(chunk)
        return self.term

    def send(self, keys, settle=0.5):
        if isinstance(keys, str): keys = keys.encode()
        os.write(self.fd, keys)
        return self.pump(settle)

    def close(self):
        try: os.close(self.fd)
        except OSError: pass
        try:
            os.kill(self.pid, 9); os.waitpid(self.pid, 0)
        except OSError: pass


PASS = FAIL = 0
def check(label, cond, detail=''):
    global PASS, FAIL
    if cond:
        PASS += 1; print(f'  ok   {label}')
    else:
        FAIL += 1; print(f'  FAIL {label}' + (f': {detail}' if detail else ''))



W = os.environ.get('EMIL_PTY_WORK', '/tmp/emil-pty')
os.makedirs(W, exist_ok=True)
SB = os.environ.get('SANDBOX', './wasix-sandbox/bin')
WASMER = os.environ.get('WASMER', 'wasmer')
args = [a for a in sys.argv[1:]]
SB_ARG = None
if '--sandbox' in args:
    k = args.index('--sandbox'); SB_ARG = args[k+1]; del args[k:k+2]
BIN = args[0] if args else 'emil.wasm'
if SB_ARG: SB = SB_ARG
# wasmer resolves --volume host paths itself; a relative one does not
# map, and the only symptom is "unable to create subprocess" when the
# editor later tries to spawn /bin/sh.
SB = os.path.abspath(SB)

def argv(target, sandbox=True):
    a = [WASMER, 'run', '--volume', f'{W}:/w']
    if sandbox:
        a += ['--volume', f'{SB}:/bin', '--env', 'PATH=/bin',
              '--env', 'TMPDIR=/tmp']
    a += ['--env', 'LANG=C.UTF-8', BIN, '--']
    if target: a += [target]
    return a

def write(name, content):
    p = os.path.join(W, name)
    with open(p, 'w') as f: f.write(content)
    return p

def read(name):
    with open(os.path.join(W, name)) as f: return f.read()

print(f'=== exploratory pty run: {BIN} ===')

# ---- 1. open a file, status bar, editing, save -----------------------
write('e1.txt', 'first line\nsecond line\nthird line\n')
s = Session(argv('/w/e1.txt'))
s.pump(1.5)
t = s.term
check('file content rendered', 'first line' in t.text(), t.line(0))
check('status bar names the file', 'e1.txt' in t.text(),
      repr(t.line(22)))
check('status bar shows position 1:0', '1:0' in t.text(), repr(t.line(22)))
# type at point
s.send('HELLO ', 0.6)
check('typed text appears', 'HELLO first line' in s.term.text(),
      s.term.line(0))
check('buffer marked dirty (**)', '**' in s.term.text(), repr(s.term.line(22)))
# save
s.send('\x18\x13', 1.2)   # C-x C-s
check('save wrote through to disk',
      read('e1.txt').startswith('HELLO first line'), repr(read('e1.txt')[:30]))
check('clean flag after save', '--' in s.term.text(), repr(s.term.line(22)))
s.close()

# ---- 2. undo / redo --------------------------------------------------
write('e2.txt', 'abc\n')
s = Session(argv('/w/e2.txt')); s.pump(1.5)
s.send('XYZ', 0.5)
check('typed XYZ', 'XYZabc' in s.term.text(), s.term.line(0))
s.send('\x1f', 0.6)         # C-_ undo
check('undo removed the burst', 'XYZ' not in s.term.text().split('\n')[0],
      s.term.line(0))
s.send('\x1f', 0.6)         # again -> nothing more, or redo chain
s.close()

# ---- 3. incremental search + highlight -------------------------------
write('e3.txt', 'alpha\nbeta\ngamma\ndelta\n')
s = Session(argv('/w/e3.txt')); s.pump(1.5)
s.send('\x13gam', 0.8)      # C-s gam
check('isearch moved to match', '3:0' in s.term.text() or '3:' in s.term.text(),
      repr(s.term.line(22)))
s.send('\r', 0.4)
s.close()

# ---- 4. shell command M-! (needs /bin/sh) ----------------------------
write('e4.txt', 'x\n')
s = Session(argv('/w/e4.txt')); s.pump(1.5)
s.send('\x1b!', 0.5)
s.send('echo wasix-shell-ok\r', 2.5)
txt = s.term.text()
check('M-! ran the command', 'wasix-shell-ok' in txt,
      [l for l in txt.split('\n') if l.strip()][:4])
check('output went to *Shell Output*', 'Shell Output' in txt,
      repr(s.term.line(22)))
s.close()

# ---- 5. M-| pipeline replacing the region ----------------------------
write('e5.txt', 'alpha\nbeta\n')
s = Session(argv('/w/e5.txt')); s.pump(1.5)
s.send('\x18h', 0.5)             # C-x h  mark whole buffer
s.send('\x15\x1b|', 0.5)         # C-u M-|
s.send('tr a-z A-Z\r', 3.0)
txt = s.term.text()
check('pipeline transformed the region', 'ALPHA' in txt,
      [l for l in txt.split('\n') if l.strip()][:3])
s.send('\x18\x13', 1.2)          # C-x C-s
check('transformed text saved', 'ALPHA' in read('e5.txt'), repr(read('e5.txt')))
s.close()

# ---- 6. UTF-8 / CJK width --------------------------------------------
write('e6.txt', '日本語テキスト\ncafé naïve\n')
s = Session(argv('/w/e6.txt')); s.pump(1.5)
t = s.term
check('CJK rendered', '日本語' in t.text(), t.line(0))
check('accented latin rendered', 'café' in t.text(), t.line(1))
# End-of-line on the CJK row should report a display column of 14, not 7 bytes
s.send('\x05', 0.5)   # C-e end of line
check('CJK end-of-line column is display cells (14)',
      '1:14' in s.term.text(), repr(s.term.line(22)))
s.close()

# ---- 7. suspend + shell drawer report unavailability ------------------
write('e7.txt', 'z\n')
s = Session(argv('/w/e7.txt')); s.pump(1.5)
s.send('\x1a', 0.8)       # C-z
check('C-z reports unavailable, editor alive',
      'not available' in s.term.text().lower(), repr(s.term.line(23)))
s.send('\x18\x1a', 0.8)   # C-x C-z shell drawer
check('C-x C-z reports unavailable, editor alive',
      'not available' in s.term.text().lower(), repr(s.term.line(23)))
s.send('abc', 0.5)
check('editor still responsive after both', 'abc' in s.term.text(),
      s.term.line(0))
s.close()

# ---- 8. window split, movement, and quit -----------------------------
write('e8.txt', ''.join(f'line{i}\n' for i in range(1, 40)))
s = Session(argv('/w/e8.txt')); s.pump(1.5)
s.send('\x182', 0.8)      # C-x 2 split
check('split produced two status bars',
      s.term.text().count('e8.txt') >= 2, s.term.text().count('e8.txt'))
s.send('\x18o', 0.5)      # C-x o other window
s.send('\x1b>', 0.6)      # M-> end of buffer
check('M-> reached end of buffer', 'Bot' in s.term.text() or 'All' in s.term.text(),
      repr(s.term.line(22)))
s.send('\x181', 0.5)      # C-x 1
s.close()

# ---- 9. clean exit ---------------------------------------------------
write('e9.txt', 'q\n')
s = Session(argv('/w/e9.txt')); s.pump(1.5)
s.send('\x18\x03', 1.5)   # C-x C-c
time.sleep(0.5)
try:
    pid, status = os.waitpid(s.pid, os.WNOHANG)
    exited = (pid != 0)
    code = os.WEXITSTATUS(status) if exited and os.WIFEXITED(status) else None
except ChildProcessError:
    exited, code = True, 0
check('C-x C-c exits cleanly', exited and (code in (0, None)),
      f'exited={exited} code={code}')
s.close()

print(f'\n{PASS} passed, {FAIL} failed')
sys.exit(1 if FAIL else 0)
