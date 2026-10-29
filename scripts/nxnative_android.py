# The desktop app of the Android port: pack your games for the nxnative app, install them on your phones and watch them run.
#   python scripts/nxnative_android.py        (needs PySide6: pip install PySide6)
# It drives scripts/build_android_from_nsp.py (its --progress json events, --info, --check, --push-only, --apk-only, --migrate) and adb:
#   Library      your games (the NSP folders you added): the picked one large, Play (on the phone picked at the top), its options and Manage; the collection below
#   Builds       the running build: its stages, progress and messages (the bar at the bottom of every page follows it); a build started from a console
#                shows here too, from the journal the script writes (<data>/builds/*.jsonl)
#   Phones       adb devices over USB and Wi-Fi (pair, connect, discover), the app and the games on each, saves backed up to the computer
#   Performance  the game's log (adb logcat) with the frame time, GPU and CPU figures it prints, a frame's GPU passes
#   Settings     your keys, and what the build needs on this computer with how to get what is missing
import json, os, re, shutil, subprocess, sys, time, uuid
from pathlib import Path

try:
    from PySide6.QtCore import QByteArray, QObject, QProcess, QProcessEnvironment, QRectF, QRunnable, QSettings, QSize, Qt, QThreadPool, QTimer, Signal  # pyright: ignore[reportMissingImports]   (PySide6 is the desktop app's alone; the lint job does not install it)
    from PySide6.QtGui import QColor, QFont, QIcon, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap  # pyright: ignore[reportMissingImports]
    from PySide6.QtSvg import QSvgRenderer  # pyright: ignore[reportMissingImports]
    from PySide6.QtWidgets import (  # pyright: ignore[reportMissingImports]
                                   QApplication, QButtonGroup, QComboBox, QDialog, QDialogButtonBox, QFileDialog, QFormLayout, QFrame, QGridLayout, QHBoxLayout,
                                   QHeaderView, QInputDialog, QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMenu, QMessageBox, QPlainTextEdit,
                                   QProgressBar, QPushButton, QScrollArea, QSplitter, QStackedWidget, QTableWidget, QTableWidgetItem, QToolButton,
                                   QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget)
except ImportError:
    sys.exit(f'nxnative_android needs PySide6: {sys.executable} -m pip install PySide6')

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'scripts' / 'build_android_from_nsp.py'
PKG = 'org.nxnative.android'
REMOTE = f'/sdcard/Android/data/{PKG}/files'
sys.path.insert(0, str(REPO / 'scripts'))
import build_android_from_nsp as ba   # noqa: E402  (data_dir, library: the host library)

LANGUAGES = [('en-US', 'American English'), ('en-GB', 'British English'), ('ja', 'Japanese'), ('fr', 'French'), ('de', 'German'), ('es-419', 'Latin American Spanish'),
             ('es', 'Spanish'), ('it', 'Italian'), ('nl', 'Dutch'), ('fr-CA', 'Canadian French'), ('pt', 'Portuguese'), ('ru', 'Russian'), ('ko', 'Korean'),
             ('zh-Hant', 'Traditional Chinese'), ('zh-Hans', 'Simplified Chinese'), ('pt-BR', 'Brazilian Portuguese')]
NOWINDOW = 0x08000000 if os.name == 'nt' else 0   # CREATE_NO_WINDOW: no console flashing up for each adb call

# ---- the palette: a dark room lit by the game's own colours; colour only where it means something ----
INK = '#0b0b0c'          # the room
PANEL = '#141416'        # a raised surface (top bar, build bar, cards' frames)
LIFT = '#1d1d20'         # hover, the picked tab
LINE = 'rgba(255,255,255,0.08)'
TEXT, TEXT2, TEXT3 = '#f4f4f5', '#a1a1aa', '#71717a'
CREAM = '#fdf3dc'        # the one primary action: Play
READY = '#3ddc84'        # Android green: on the phone, ready to play
BUILD = '#f5b82e'        # a build running
ERR = '#f06a6a'
FONT = 'Segoe UI Variable Display' if os.name == 'nt' else 'Inter'

ICONS = {   # 24x24 strokes (lucide), drawn in the label's colour
    'library': '<line x1="6" x2="10" y1="11" y2="11"/><line x1="8" x2="8" y1="9" y2="13"/><line x1="15" x2="15.01" y1="12" y2="12"/><line x1="18" x2="18.01" y1="10" y2="10"/><path d="M17.32 5H6.68a4 4 0 0 0-3.978 3.59c-.006.052-.01.101-.017.152C2.604 9.416 2 14.456 2 16a3 3 0 0 0 3 3c1 0 1.5-.5 2-1l1.414-1.414A2 2 0 0 1 9.828 16h4.344a2 2 0 0 1 1.414.586L17 18c.5.5 1 1 2 1a3 3 0 0 0 3-3c0-1.545-.604-6.584-.685-7.258-.007-.05-.011-.1-.017-.151A4 4 0 0 0 17.32 5z"/>',
    'builds': '<path d="M11 21.73a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73z"/><path d="M12 22V12"/><path d="m3.3 7 7.703 4.734a2 2 0 0 0 1.994 0L20.7 7"/><path d="m7.5 4.27 9 5.15"/>',
    'phone': '<rect width="14" height="20" x="5" y="2" rx="2" ry="2"/><path d="M12 18h.01"/>',
    'perf': '<path d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2"/>',
    'settings': '<line x1="21" x2="14" y1="4" y2="4"/><line x1="10" x2="3" y1="4" y2="4"/><line x1="21" x2="12" y1="12" y2="12"/><line x1="8" x2="3" y1="12" y2="12"/><line x1="21" x2="16" y1="20" y2="20"/><line x1="12" x2="3" y1="20" y2="20"/><line x1="14" x2="14" y1="2" y2="6"/><line x1="8" x2="8" y1="10" y2="14"/><line x1="16" x2="16" y1="18" y2="22"/>',
    'search': '<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
    'plus': '<path d="M5 12h14"/><path d="M12 5v14"/>',
    'more': '<circle cx="12" cy="12" r="10"/><path d="M17 12h.01"/><path d="M12 12h.01"/><path d="M7 12h.01"/>',
    'down': '<path d="m6 9 6 6 6-6"/>', 'right': '<path d="m9 18 6-6-6-6"/>', 'left': '<path d="m15 18-6-6 6-6"/>',
    'tools': '<path d="m15 12-8.373 8.373a1 1 0 1 1-3-3L12 9"/><path d="m18 15 4-4"/><path d="m21.5 11.5-1.914-1.914A2 2 0 0 1 19 8.172V7l-2.26-1.13a3.004 3.004 0 0 0-3.5.53l-.32.32"/><path d="M3 3l4 4"/>',
}
PLAY_SVG = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="{c}" d="M7 4.5v15a1 1 0 0 0 1.52.85l12-7.5a1 1 0 0 0 0-1.7l-12-7.5A1 1 0 0 0 7 4.5z"/></svg>'


def icon(name, color=TEXT2, size=20):
    svg = PLAY_SVG.format(c=color) if name == 'play' else (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="{color}" stroke-width="2" '
                                                         f'stroke-linecap="round" stroke-linejoin="round">{ICONS[name]}</svg>')
    pm = QPixmap(size * 2, size * 2); pm.fill(Qt.transparent)
    p = QPainter(pm); QSvgRenderer(QByteArray(svg.encode())).render(p); p.end(); pm.setDevicePixelRatio(2)
    return QIcon(pm)


STYLE = f"""
* {{ font-family: '{FONT}', 'Segoe UI', sans-serif; color: {TEXT}; }}
QMainWindow, #page {{ background: {INK}; }}
QWidget#topbar, QWidget#buildbar {{ background: {PANEL}; }}
QLabel#wordmark {{ font-size: 22px; font-weight: 700; letter-spacing: -0.5px; padding-right: 18px; }}
QToolButton#nav {{ background: transparent; border: none; border-radius: 10px; padding: 8px 14px; font-size: 14px; font-weight: 600; color: {TEXT2}; }}
QToolButton#nav:hover {{ background: {LIFT}; color: {TEXT}; }}
QToolButton#nav:checked {{ background: {LIFT}; color: {TEXT}; }}
QPushButton {{ background: {LIFT}; border: 1px solid {LINE}; border-radius: 9px; padding: 8px 14px; font-size: 13px; font-weight: 600; }}
QPushButton:hover {{ background: #26262a; }}
QPushButton:pressed {{ background: #2e2e33; }}
QPushButton:disabled {{ color: {TEXT3}; background: {PANEL}; }}
QPushButton#ghost {{ background: transparent; border: 1px solid {LINE}; color: {TEXT2}; }}
QPushButton#ghost:hover {{ color: {TEXT}; background: {LIFT}; }}
QPushButton#play {{ background: {CREAM}; color: #121212; border: none; border-radius: 26px; padding: 0 38px; min-height: 52px; font-size: 17px; font-weight: 700; }}
QPushButton#play:hover {{ background: #fff8e8; }}
QPushButton#play:pressed {{ background: #efe3c6; }}
QPushButton#play:disabled {{ background: #3a3a3f; color: {TEXT3}; }}
QPushButton#round {{ background: rgba(20,20,22,0.55); border: 1px solid rgba(255,255,255,0.18); border-radius: 26px; min-width: 52px; max-width: 52px; min-height: 52px; max-height: 52px; padding: 0; }}
QPushButton#round:hover {{ background: rgba(40,40,44,0.75); }}
QToolButton#manage {{ background: transparent; border: none; font-size: 14px; font-weight: 600; padding: 8px 6px; }}
QToolButton#manage:hover {{ color: white; }}
QToolButton#manage::menu-indicator {{ image: none; }}
QPushButton#stop {{ background: transparent; border: 1px solid {BUILD}; color: {BUILD}; }}
QPushButton#stop:hover {{ background: rgba(245,184,46,0.12); }}
QPushButton#arrow {{ background: rgba(20,20,22,0.85); border: 1px solid rgba(255,255,255,0.16); border-radius: 22px; min-width: 44px; max-width: 44px; min-height: 44px; max-height: 44px; padding: 0; }}
QLineEdit, QComboBox {{ background: #09090a; border: 1px solid {LINE}; border-radius: 8px; padding: 7px 10px; font-size: 13px; selection-background-color: #3a3a40; }}
QLineEdit:focus, QComboBox:focus {{ border: 1px solid rgba(255,255,255,0.28); }}
QComboBox QAbstractItemView {{ background: {LIFT}; border: 1px solid {LINE}; selection-background-color: #2e2e33; }}
QMenu {{ background: {LIFT}; border: 1px solid {LINE}; border-radius: 10px; padding: 6px; }}
QMenu::item {{ padding: 8px 18px; border-radius: 6px; font-size: 13px; }}
QMenu::item:selected {{ background: #2e2e33; }}
QMenu::item:disabled {{ color: {TEXT3}; }}
QMenu::separator {{ height: 1px; background: {LINE}; margin: 5px 8px; }}
QProgressBar {{ background: #26262a; border: none; border-radius: 4px; max-height: 8px; min-height: 8px; text-align: right; }}
QProgressBar::chunk {{ background: {BUILD}; border-radius: 4px; }}
QTableWidget, QTreeWidget, QListWidget, QPlainTextEdit {{ background: {PANEL}; border: 1px solid {LINE}; border-radius: 10px; gridline-color: transparent; font-size: 13px; }}
QTableWidget::item, QTreeWidget::item, QListWidget::item {{ padding: 6px; }}
QTableWidget::item:selected, QTreeWidget::item:selected, QListWidget::item:selected {{ background: #26262a; }}
QHeaderView::section {{ background: {PANEL}; color: {TEXT3}; border: none; border-bottom: 1px solid {LINE}; padding: 8px; font-size: 11px; font-weight: 600; text-transform: uppercase; }}
QScrollArea {{ background: transparent; border: none; }}
QScrollBar:vertical {{ background: transparent; width: 10px; }} QScrollBar::handle:vertical {{ background: #2e2e33; border-radius: 5px; min-height: 30px; }}
QScrollBar:horizontal {{ height: 0px; }} QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; width: 0; }}
QLabel#h1 {{ font-size: 28px; font-weight: 700; letter-spacing: -0.6px; }}
QLabel#h2 {{ font-size: 26px; font-weight: 700; letter-spacing: -0.5px; }}
QLabel#muted {{ color: {TEXT3}; font-size: 13px; }}
QLabel#meta {{ color: {TEXT2}; font-size: 13px; }}
QLabel#tilelabel {{ color: {TEXT3}; font-size: 11px; font-weight: 600; letter-spacing: 0.6px; }}
QLabel#tilevalue {{ font-size: 26px; font-weight: 700; letter-spacing: -0.5px; }}
QFrame#tile {{ background: {PANEL}; border: 1px solid {LINE}; border-radius: 12px; }}
QFrame#card {{ background: {PANEL}; border: 1px solid {LINE}; border-radius: 12px; }}
"""


def adb_run(serial, *args, timeout=20):   # → (ok, output); never raises
    cmd = ['adb'] + (['-s', serial] if serial else []) + [str(a) for a in args]
    try: r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, creationflags=NOWINDOW, env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    except (OSError, subprocess.TimeoutExpired) as e: return False, str(e)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


class Job(QRunnable):   # a blocking function on the thread pool; its result comes back on the UI thread
    class Sig(QObject): done = Signal(object)
    live = set()   # the jobs in flight: Python would free a job (and its signal object) as soon as bg() returned
    def __init__(self, fn, cb):
        super().__init__(); self.setAutoDelete(False); self.fn, self.cb, self.sig = fn, cb, Job.Sig(); self.sig.done.connect(self.finish)
    def run(self):
        try: r = self.fn()
        except Exception as e: r = e
        self.sig.done.emit(r)
    def finish(self, r):
        Job.live.discard(self); self.cb(r)


def bg(fn, cb): j = Job(fn, cb); Job.live.add(j); QThreadPool.globalInstance().start(j)


def alive(pid):
    if pid <= 0: return False
    if os.name != 'nt':
        try: os.kill(pid, 0); return True
        except OSError: return False
    import ctypes; k = ctypes.windll.kernel32; h = k.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h: return False
    code = ctypes.c_ulong(); k.GetExitCodeProcess(h, ctypes.byref(code)); k.CloseHandle(h); return code.value == 259   # STILL_ACTIVE


def journal_title(argv):   # what a build from the console does, from its command line
    if '--apk-only' in argv: return 'Update the app'
    if '--migrate' in argv: return 'Move the old install'
    folder = next((x for x in argv if not x.startswith('-') and Path(x).is_dir()), None)
    name = None
    if folder:
        try: name = next((m.get('name') for _, (_, m) in ba.library().items() if m.get('nsp_dir') == str(Path(folder).resolve())), None)
        except Exception: pass
    return ('Install ' if '--push-only' in argv else 'Build ') + (name or (Path(folder).name if folder else 'a game'))


def label(text, name=None, wrap=False):
    l = QLabel(text)
    if name: l.setObjectName(name)
    l.setWordWrap(wrap); return l


def gb_text(n): return f'{n / 2**30:.1f} GB' if n and n >= 2**30 else f'{(n or 0) / 2**20:.0f} MB'


def esc(s): return s.replace('&', '&amp;').replace('<', '&lt;')


def clock_text(sec): sec = int(sec or 0); return f'{sec // 60}:{sec % 60:02d}' if sec >= 60 else f'{sec} s'


def count_text(done, total, unit):   # "1,234 / 37,393 files", or bytes as "1.2 / 3.7 GB" (unit 'B'), with the percentage
    pct = f' ({100 * done // total}%)' if total else ''
    if unit == 'B':
        u, d = next((u, d) for u, d in (('GB', 1 << 30), ('MB', 1 << 20), ('KB', 1 << 10), ('B', 1)) if total >= d or d == 1)
        return f'{done / d:.1f} / {total / d:.1f} {u}{pct}'
    return f'{done:,} / {total:,} {unit}'.rstrip() + pct


def wash(pm, w, h):   # the art's colour wash: the picture shrunk to a few pixels and spread over w x h (a soft blur of its own colours)
    if pm.isNull(): return QPixmap()
    return pm.scaled(12, 12, Qt.IgnoreAspectRatio, Qt.SmoothTransformation).scaled(w, h, Qt.IgnoreAspectRatio, Qt.SmoothTransformation)


# ---------------- a game, as the window shows it ----------------
class Game:
    def __init__(self, folder, info):
        self.folder, self.info = folder, info
        ex = info.get('extracted') or {}; self.newest = info.get('newest')
        e = ex.get(self.newest) or (next(iter(ex.values())) if ex else {})
        self.id = info.get('title_id'); self.name = e.get('name') or info.get('name') or self.id or Path(folder).name; self.error = info.get('error')
        self.icon = e.get('icon'); self.display = e.get('display'); self.languages = e.get('languages') or [c for c, _ in LANGUAGES]
        self.profile = info.get('profile'); self.provisional = info.get('provisional'); self.host = info.get('host'); self.tested = info.get('android_tested')
        self.pm = QPixmap(self.icon) if self.icon else QPixmap()

    def status(self, on_phone, phone):   # (text, colour, state): state picks what Play does
        if self.error: return f'Could not read the folder: {self.error}', ERR, 'none'
        if not self.profile: return 'No profile: experimental build only', ERR, 'survey'
        x = 'Experimental (provisional profile, may not boot) · ' if self.provisional else ''   # an experimental build's profile, guessed from the survey
        if self.id in on_phone and self.host: return f'{x}Ready on {phone or "the phone"}', ERR if x else READY, 'play'
        if self.host: return f'{x}Built · not on this phone', ERR if x else BUILD, 'install'
        return f'{x}Not built yet', ERR if x else TEXT3, 'build'


class Hero(QWidget):   # the picked game, large: its art's wash behind, the art itself on the right, the status, the title and the actions
    def __init__(self):
        super().__init__(); self.setMinimumHeight(330); self.pm = QPixmap(); self.cache = None
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 44, 48, 32); lay.setSpacing(0)
        self.status = label('', wrap=False); self.status.setStyleSheet('font-size: 13px; font-weight: 700; letter-spacing: 1.4px')
        lay.addWidget(self.status); lay.addSpacing(18)
        self.title = label('', wrap=True); self.title.setStyleSheet('font-size: 54px; font-weight: 800; letter-spacing: -1.6px; line-height: 1.05')
        self.title.setMaximumWidth(900); lay.addWidget(self.title); lay.addSpacing(10)
        self.meta = label('', 'meta'); lay.addWidget(self.meta); lay.addStretch(); lay.addSpacing(20)
        row = QHBoxLayout(); row.setSpacing(16)
        self.play = QPushButton('  Play'); self.play.setObjectName('play'); self.play.setIcon(icon('play', '#121212', 22)); self.play.setIconSize(QSize(22, 22)); self.play.setCursor(Qt.PointingHandCursor)
        self.opts = QPushButton(); self.opts.setObjectName('round'); self.opts.setIcon(icon('settings', TEXT, 22)); self.opts.setIconSize(QSize(22, 22)); self.opts.setToolTip('Version and language'); self.opts.setCursor(Qt.PointingHandCursor)
        self.manage = QToolButton(); self.manage.setObjectName('manage'); self.manage.setText(' Manage  '); self.manage.setIcon(icon('more', TEXT, 18)); self.manage.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.manage.setPopupMode(QToolButton.InstantPopup); self.manage.setCursor(Qt.PointingHandCursor)
        row.addWidget(self.play); row.addWidget(self.opts); row.addWidget(self.manage); row.addStretch(); lay.addLayout(row)

    def show_game(self, g, status, color):
        self.pm = g.pm if g else QPixmap(); self.cache = None
        self.status.setText(f'<span style="color:{color}">●&nbsp;&nbsp;{esc(status.upper())}</span>' if g else '')
        self.title.setText(esc(g.name) if g else 'Add your first game')
        if g:
            bits = [g.id, f'v{g.display}' if g.display else None, (g.info.get('table') or {}).get('engine'), 'played on Android' if g.tested else 'not tested on Android yet']
            self.meta.setText('   ·   '.join(b for b in bits if b))
        else: self.meta.setText('Pick the folder with your game\'s NSP files and your prod.keys: + Build game, at the top right')
        self.update()

    def resizeEvent(self, e):
        self.cache = None; px = 54 if e.size().height() >= 440 else 42
        self.title.setStyleSheet(f'font-size: {px}px; font-weight: 800; letter-spacing: -1.6px'); super().resizeEvent(e)

    def paintEvent(self, _):
        w, h = self.width(), self.height()
        if self.cache is None or self.cache.size() != QSize(w, h):
            pm = QPixmap(w, h); pm.fill(QColor(INK)); p = QPainter(pm); p.setRenderHint(QPainter.Antialiasing); p.setRenderHint(QPainter.SmoothPixmapTransform)
            if not self.pm.isNull():
                p.setOpacity(0.85); p.drawPixmap(0, 0, wash(self.pm, w, h)); p.setOpacity(1)
                art = int(h * 0.82); x, y = w - art - 64, (h - art) // 2   # the game's own picture, sharp, on the right
                path = QPainterPath(); path.addRoundedRect(QRectF(x, y, art, art), 22, 22)
                p.save(); p.setClipPath(path); p.drawPixmap(x, y, art, art, self.pm); p.restore()
                p.setPen(QPen(QColor(255, 255, 255, 26), 1)); p.drawPath(path)
            g = QLinearGradient(0, 0, w, 0)   # the text side darkened so the title reads over any art
            g.setColorAt(0, QColor(11, 11, 12, 245)); g.setColorAt(0.45, QColor(11, 11, 12, 190)); g.setColorAt(0.75, QColor(11, 11, 12, 40)); g.setColorAt(1, QColor(11, 11, 12, 0))
            p.fillRect(0, 0, w, h, g)
            v = QLinearGradient(0, h * 0.55, 0, h); v.setColorAt(0, QColor(11, 11, 12, 0)); v.setColorAt(1, QColor(11, 11, 12, 255)); p.fillRect(0, 0, w, h, v)
            p.end(); self.cache = pm
        QPainter(self).drawPixmap(0, 0, self.cache)


class Card(QWidget):   # one game of the collection: its art in a 16:9 frame (the wash, the picture centred), its name and status
    clicked = Signal(object)
    W, H = 300, 169

    def __init__(self, g):
        super().__init__(); self.g = g; self.picked = False; self.hover = False; self.status = ('', TEXT3)
        self.setFixedSize(self.W, self.H + 78); self.setCursor(Qt.PointingHandCursor); self.setMouseTracking(True)
        self.art = None

    def set_status(self, text, color): self.status = (text, color); self.update()
    def set_picked(self, on): self.picked = on; self.update()
    def enterEvent(self, e): self.hover = True; self.update()
    def leaveEvent(self, e): self.hover = False; self.update()
    def mousePressEvent(self, e): self.clicked.emit(self.g)

    def paintEvent(self, _):
        p = QPainter(self); p.setRenderHint(QPainter.Antialiasing); p.setRenderHint(QPainter.SmoothPixmapTransform)
        r = QRectF(0.5, 0.5, self.W - 1, self.H - 1); path = QPainterPath(); path.addRoundedRect(r, 10, 10)
        p.save(); p.setClipPath(path)
        if self.art is None:
            self.art = QPixmap(self.W, self.H); self.art.fill(QColor(LIFT)); q = QPainter(self.art); q.setRenderHint(QPainter.SmoothPixmapTransform)
            if not self.g.pm.isNull():
                q.drawPixmap(0, 0, wash(self.g.pm, self.W, self.H)); q.fillRect(0, 0, self.W, self.H, QColor(0, 0, 0, 60))
                s = int(self.H * 0.86); q.drawPixmap((self.W - s) // 2, (self.H - s) // 2, s, s, self.g.pm)
            else:   # no art until the game is extracted (the first build's first stages): its name
                gr = QLinearGradient(0, 0, self.W, self.H); gr.setColorAt(0, QColor('#232327')); gr.setColorAt(1, QColor('#141416')); q.fillRect(0, 0, self.W, self.H, gr)
                f = QFont(); f.setFamilies([FONT, 'Segoe UI']); f.setPixelSize(22); f.setWeight(QFont.Bold); q.setFont(f); q.setPen(QColor(TEXT2))
                q.drawText(QRectF(20, 0, self.W - 40, self.H), Qt.AlignCenter | Qt.TextWordWrap, self.g.name)
            q.end()
        p.drawPixmap(0, 0, self.art)
        if self.hover and not self.picked: p.fillRect(r, QColor(255, 255, 255, 14))
        p.restore()
        p.setPen(QPen(QColor(255, 255, 255, 235) if self.picked else QColor(255, 255, 255, 22), 2 if self.picked else 1)); p.drawPath(path)
        f = QFont(); f.setFamilies([FONT, 'Segoe UI']); f.setPixelSize(17); f.setWeight(QFont.DemiBold); p.setFont(f); p.setPen(QColor(TEXT))
        p.drawText(QRectF(0, self.H + 12, self.W, 24), Qt.AlignLeft | Qt.AlignVCenter, p.fontMetrics().elidedText(self.g.name, Qt.ElideRight, self.W))
        text, color = self.status
        p.setPen(Qt.NoPen); p.setBrush(QColor(color)); p.drawEllipse(QRectF(1, self.H + 46, 8, 8))
        f.setPixelSize(14); f.setWeight(QFont.Normal); p.setFont(f); p.setPen(QColor(color if color in (READY,) else TEXT2))
        p.drawText(QRectF(18, self.H + 38, self.W - 18, 24), Qt.AlignLeft | Qt.AlignVCenter, text)
        if self.picked: p.setPen(Qt.NoPen); p.setBrush(QColor(TEXT)); p.drawRoundedRect(QRectF(0, self.H + 72, self.W, 3), 1.5, 1.5)


# ---------------- Library ----------------
class LibraryPage(QWidget):
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page'); self.games = {}; self.cards = []; self.picked = None; self.filter = ''
        page = QVBoxLayout(self); page.setContentsMargins(0, 0, 0, 0)
        self.vscroll = QScrollArea(); self.vscroll.setWidgetResizable(True); self.vscroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); page.addWidget(self.vscroll)
        inner = QWidget(); inner.setObjectName('page'); self.vscroll.setWidget(inner)   # too low a window scrolls instead of piling the collection onto the hero
        outer = QVBoxLayout(inner); outer.setContentsMargins(0, 0, 0, 0); outer.setSpacing(0)
        self.hero = Hero(); outer.addWidget(self.hero, 1)
        self.hero.play.clicked.connect(self.play); self.hero.opts.clicked.connect(self.options)
        self.menu = QMenu(self); self.hero.manage.setMenu(self.menu); self.menu.aboutToShow.connect(self.fill_menu)
        body = QWidget(); bl = QVBoxLayout(body); bl.setContentsMargins(48, 8, 48, 24); bl.setSpacing(18)
        head = QHBoxLayout(); head.addWidget(label('Your collection', 'h2')); head.addStretch(); self.count = label('', 'muted'); head.addWidget(self.count); bl.addLayout(head)
        strip = QWidget(); sl = QGridLayout(strip); sl.setContentsMargins(0, 0, 0, 0)
        self.scroll = QScrollArea(); self.scroll.setWidgetResizable(True); self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); self.scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.scroll.setFixedHeight(Card.H + 84)
        self.row = QWidget(); self.row.setStyleSheet('background: transparent'); self.rl = QHBoxLayout(self.row); self.rl.setContentsMargins(0, 0, 0, 0); self.rl.setSpacing(20); self.rl.addStretch()
        self.scroll.setWidget(self.row); sl.addWidget(self.scroll, 0, 0); self.scroll.viewport().installEventFilter(self)
        self.next = QPushButton(); self.next.setObjectName('arrow'); self.next.setIcon(icon('right', TEXT, 20)); self.next.clicked.connect(lambda: self.slide(1))
        self.prev = QPushButton(); self.prev.setObjectName('arrow'); self.prev.setIcon(icon('left', TEXT, 20)); self.prev.clicked.connect(lambda: self.slide(-1))
        sl.addWidget(self.next, 0, 0, Qt.AlignRight | Qt.AlignTop); sl.addWidget(self.prev, 0, 0, Qt.AlignLeft | Qt.AlignTop)
        self.next.setStyleSheet(f'margin-top: {Card.H // 2 - 22}px'); self.prev.setStyleSheet(f'margin-top: {Card.H // 2 - 22}px')
        bl.addWidget(strip); self.empty = label('No games yet. + Build game adds the folder with a game\'s NSP files.', 'muted'); bl.addWidget(self.empty)
        outer.addWidget(body, 0)
        self.scroll.horizontalScrollBar().valueChanged.connect(self.arrows); self.scroll.horizontalScrollBar().rangeChanged.connect(lambda *_: self.arrows())
        self.reload()

    def folders(self): return [f for f in (self.win.st.value('folders', []) or []) if f]
    def reload(self):
        fs = list(self.folders()); seen = {str(Path(f).resolve()) for f in fs}   # resolved: a folder behind a link (Downloads moved to another drive) is the same folder
        for gid, (_, meta) in ba.library().items():   # packed on this computer from a folder not in the list
            if meta.get('nsp_dir') and str(Path(meta['nsp_dir']).resolve()) not in seen: fs.append(meta['nsp_dir']); seen.add(str(Path(meta['nsp_dir']).resolve()))
        if not fs: self.render(); return
        def read():
            out = {}
            for f in fs:
                if not Path(f).is_dir(): continue
                try: out[f] = self.win.script_json('--info', f)
                except Exception as e: out[f] = {'error': str(e).strip().splitlines()[-1] if str(e).strip() else 'could not be read', 'name': Path(f).name}
            return out
        def got(r):
            if isinstance(r, Exception): self.win.toast(f'Reading the games failed: {r}'); r = {}
            self.games, ids = {}, set()
            for f, i in r.items():   # one card per game, from the first folder that holds it
                g = Game(f, i)
                if g.id and g.id in ids: continue
                ids.add(g.id); self.games[f] = g
            self.render()
        bg(read, got)
    def render(self):
        while self.rl.count() > 1:
            w = self.rl.takeAt(0).widget()
            if w: w.deleteLater()
        self.cards = []
        gs = [g for g in self.games.values() if self.filter.lower() in g.name.lower()]
        for g in gs:
            c = Card(g); c.clicked.connect(self.pick); self.rl.insertWidget(self.rl.count() - 1, c); self.cards.append(c)
        n = len(self.games); self.count.setText(f'{n} game' + ('' if n == 1 else 's')); self.empty.setVisible(not n)
        self.refresh_status()
        keep = self.picked if self.picked in self.games else self.win.st.value('picked', None)
        g = self.games.get(keep) or (gs[0] if gs else None)
        self.pick(g); QTimer.singleShot(0, self.arrows)
    def refresh_status(self):
        on, phone = self.win.phone_games, self.win.phone_name()
        for c in self.cards: t, col, _ = c.g.status(on, phone); c.set_status(t, col)
        if self.picked in self.games: self.show_hero(self.games[self.picked])
    def pick(self, g):
        self.picked = g.folder if g else None
        if g: self.win.st.setValue('picked', g.folder)
        for c in self.cards: c.set_picked(c.g is g)
        self.show_hero(g)
    def show_hero(self, g):
        if not g: self.hero.show_game(None, '', TEXT3); self.hero.play.setEnabled(False); self.hero.opts.setEnabled(False); return
        text, color, state = g.status(self.win.phone_games, self.win.phone_name())
        self.hero.show_game(g, text, color); self.hero.opts.setEnabled(bool(g.profile))
        self.hero.play.setText({'play': '  Play', 'install': '  Install on phone', 'build': '  Build and install', 'survey': '  Experimental build', 'none': '  Not supported'}[state])
        self.hero.play.setEnabled(state != 'none')
    def current(self): return self.games.get(self.picked)
    def slide(self, d): sb = self.scroll.horizontalScrollBar(); sb.setValue(sb.value() + d * (Card.W + 20) * 2)
    def arrows(self):
        sb = self.scroll.horizontalScrollBar(); self.next.setVisible(sb.value() < sb.maximum()); self.prev.setVisible(sb.value() > 0)
    def eventFilter(self, obj, e):   # the wheel over the collection scrolls it sideways
        if e.type() == e.Type.Wheel and obj is self.scroll.viewport():
            sb = self.scroll.horizontalScrollBar()
            if sb.maximum() > 0: sb.setValue(sb.value() - e.angleDelta().y()); return True
        return super().eventFilter(obj, e)

    # ---- actions ----
    def play(self):
        g = self.current()
        if not g: return
        state = g.status(self.win.phone_games, self.win.phone_name())[2]
        s = self.win.serial()
        if state in ('play', 'install') and not s: self.win.toast('Connect a phone first (Phones)'); return
        if state == 'play':
            bg(lambda: adb_run(s, 'shell', 'am', 'start', '-n', f'{PKG}/.Launcher', '--es', 'game', g.id, '--ez', 'play', 'true'),
               lambda r: self.win.toast(f'Starting {g.name} on {self.win.phone_name()}' if not isinstance(r, Exception) and r[0] else f'Could not start it: {r}'))
        elif state == 'install': self.run(g, ['--push-only'], 'Install')
        elif state == 'build': self.run(g, [], 'Build') if s else self.run(g, ['--no-push'], 'Build')
        elif state == 'survey':   # no profile: the script surveys the game, guesses a provisional profile and packs a best-effort APK (not installed: it may not boot)
            box = QMessageBox(QMessageBox.Warning, 'Experimental build', f'{g.name} has no profile in nxnative, so it is not supported yet. '
                              'The build extracts the game, writes a survey of it (survey.md in its folder), guesses a provisional profile and packs an APK that '
                              'will most likely not boot. Nothing is installed on the phone. It is the starting point for porting the game.', parent=self)
            go = box.addButton('Build anyway', QMessageBox.AcceptRole); box.setDefaultButton(box.addButton(QMessageBox.Cancel)); box.exec()
            if box.clickedButton() is go: self.run(g, ['--no-push'], 'Experimental build')
    def run(self, g, extra, what):
        keys = self.win.st.value('keys', '')
        if not keys or not Path(keys).is_file(): self.win.toast('Pick your prod.keys in Settings first'); self.win.go('settings'); return
        args = [g.folder, '-k', keys, '-l', self.win.st.value(f'lang/{g.id}', 'en-US') or 'en-US']
        v = self.win.st.value(f'ver/{g.id}', '')
        if v: args += ['-p', v]
        s = self.win.serial()
        if '--no-push' not in extra:
            if not s: self.win.toast('Connect a phone first (Phones)'); return
            if self.win.phone_legacy and not self.legacy_ok(s): return
            args += ['--serial', s]
        self.win.builds.start(f'{what} {g.name}', args + extra, after=self.after_task)
    def after_task(self): self.win.refresh_phone(); self.reload()
    def legacy_ok(self, s):   # the phone holds the one-game install (files/game) and its save: installing moves it to the launcher, and the app copies the save on its next start
        b = QMessageBox(QMessageBox.Warning, 'This phone has a save', f'{self.win.phone_name()} has the app from before the launcher, with its game and save. Installing updates the app '
                        '(its data stays), moves the game folder into the launcher and the app copies the save to the game the next time it opens; the old save is left as it was.\n\n'
                        'Back up its saves first (Phones), and try this on a phone whose save you can lose before the one you care about.', QMessageBox.Cancel, self)
        go = b.addButton('Install anyway', QMessageBox.DestructiveRole); b.setDefaultButton(QMessageBox.Cancel); b.exec()
        return b.clickedButton() is go
    def options(self):
        g = self.current()
        if not g: return
        d = QDialog(self); d.setWindowTitle(g.name); d.setStyleSheet(STYLE + f'QDialog {{ background: {PANEL}; }}'); f = QFormLayout(d); f.setContentsMargins(24, 24, 24, 24); f.setSpacing(14)
        ver = QComboBox(); ex = g.info.get('extracted') or {}
        for v in reversed(g.info.get('versions') or []): ver.addItem(f'{v}' + (f'  ({ex[v]["display"]})' if (ex.get(v) or {}).get('display') else ''), v)
        i = ver.findData(self.win.st.value(f'ver/{g.id}', '') or g.newest); ver.setCurrentIndex(max(i, 0))
        lang = QComboBox()
        for c, n in LANGUAGES:
            if c in g.languages: lang.addItem(f'{n} ({c})', c)
        i = lang.findData(self.win.st.value(f'lang/{g.id}', 'en-US')); lang.setCurrentIndex(max(i, 0))
        f.addRow(label('Version to pack', 'meta'), ver); f.addRow(label('Game language', 'meta'), lang)
        f.addRow(label('The language is the default on the phone; the app\'s settings screen changes it too.', 'muted', wrap=True))
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel); bb.accepted.connect(d.accept); bb.rejected.connect(d.reject); f.addRow(bb)
        if d.exec(): self.win.st.setValue(f'ver/{g.id}', ver.currentData()); self.win.st.setValue(f'lang/{g.id}', lang.currentData())
    def fill_menu(self):
        self.menu.clear(); g = self.current(); s = self.win.serial()
        if not g: self.menu.addAction('Add a game…', self.win.add_game); return
        def act(text, fn, on=True): a = self.menu.addAction(text, fn); a.setEnabled(on); return a
        act('Build and install', lambda: self.run(g, [], 'Build'), bool(g.profile and s))
        act('Build only', lambda: self.run(g, ['--no-push'], 'Build'), bool(g.profile))
        act('Install on phone (no build)', lambda: self.run(g, ['--push-only'], 'Install'), bool(g.profile and g.host and s))
        self.menu.addSeparator()
        act('Update the app on the phone', lambda: self.win.builds.start('Update the app', ['--apk-only', '--serial', s], after=self.after_task), bool(s and ba.library()))
        act('Remove from the phone…', lambda: self.win.phones.remove_game(g.id, g.name), bool(s and g.id in self.win.phone_games))
        self.menu.addSeparator()
        act('Open the game folder', lambda: os.startfile(g.folder) if os.name == 'nt' else None)
        act('Remove from the collection…', lambda: self.remove(g))
    def remove(self, g):
        if QMessageBox.question(self, 'Remove', f'Remove {g.name} from the collection? Its files are not deleted.') != QMessageBox.Yes: return
        if g.id in ba.library() and QMessageBox.question(self, 'Remove', 'Also take it out of the app (its host on this computer)? The next app update leaves it out; '
                                                         'the game files and the save on the phone stay.') == QMessageBox.Yes:
            shutil.rmtree(ba.data_dir() / 'hosts' / g.id, ignore_errors=True)
        self.win.st.setValue('folders', [f for f in self.folders() if f != g.folder]); self.games.pop(g.folder, None); self.render()


# ---------------- Builds ----------------
class BuildsPage(QWidget):
    progress = Signal(str, str, float, bool)   # title, detail, fraction (-1: unknown), running
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page'); self.proc = None; self.after = None; self.items = {}; self.title = ''; self.buf = ''
        self.ids = set(); self.ext = None   # this app's builds (their journals are skipped: their events come on stdout); the console build followed: {path, pid, pos}
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        self.head = label('Builds', 'h1'); lay.addWidget(self.head); self.sub = label('Nothing running. Builds start from the Library.', 'muted'); lay.addWidget(self.sub)
        self.tree = QTreeWidget(); self.tree.setHeaderLabels(['Stage', 'Progress', 'Time', 'Result']); self.tree.setColumnWidth(0, 300); self.tree.setColumnWidth(1, 300)
        self.tree.setRootIsDecorated(False); lay.addWidget(self.tree, 2)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True); self.log.setFont(QFont('Cascadia Mono', 9)); lay.addWidget(self.log, 3)
        row = QHBoxLayout(); b = QPushButton('Open the app folder'); b.setObjectName('ghost'); b.clicked.connect(lambda: os.startfile(str(ba.data_dir())) if os.name == 'nt' else None)
        row.addWidget(b); row.addStretch(); lay.addLayout(row)
        self.live = None   # (the running stage's row, its detail, its fraction or -1)
        self.jt = QTimer(self); self.jt.timeout.connect(self.follow); self.jt.timeout.connect(self.tick); self.jt.start(1000)
    def busy(self): return self.proc is not None and self.proc.state() != QProcess.NotRunning
    def follow(self):   # a build from the console: the newest journal with no exit event whose process runs
        if self.busy(): return
        if self.ext is None:
            d = ba.data_dir() / 'builds'
            for f in (sorted(d.glob('*.jsonl'), reverse=True)[:3] if d.is_dir() else []):
                try:
                    if time.time() - f.stat().st_mtime > 6 * 3600: break
                    text = f.read_text(encoding='utf-8', errors='replace'); first = json.loads(text.split('\n', 1)[0])
                except (OSError, ValueError): continue
                if first.get('id') in self.ids or '"ev": "exit"' in text or not alive(int(first.get('pid') or 0)): continue
                self.ext = {'path': f, 'pid': int(first['pid']), 'pos': 0}; self.title = journal_title(first.get('argv') or [])
                self.head.setText(self.title + '  ·  from the console'); self.sub.setText('following its journal'); self.tree.clear(); self.items = {}; self.log.clear(); self.buf = ''
                self.t0 = first.get('t') or time.time(); self.say('$ build_android_from_nsp.py ' + ' '.join(first.get('argv') or []), TEXT3)
                self.progress.emit(self.title, 'running', -1, True); break
            if self.ext is None: return
        try:
            with open(self.ext['path'], 'rb') as fh: fh.seek(self.ext['pos']); data = fh.read()
        except OSError: data = b''
        end = data.rfind(b'\n') + 1; self.ext['pos'] += end   # whole lines only: the last may still be written
        done = None
        for line in data[:end].decode('utf-8', 'replace').splitlines():
            try: ev = json.loads(line)
            except ValueError: continue
            if ev.get('ev') == 'exit': done = ev.get('code', 1)
            elif ev.get('ev') != 'start': self.feed(line)
        if done is None and not alive(self.ext['pid']): done = -1
        if done is not None:
            ok = done == 0; self.ext = None; self.live = None
            self.say('— the console build ' + ('finished' if ok else 'stopped' if done == -1 else f'failed (exit {done})'), READY if ok else ERR)
            self.sub.setText('done' if ok else 'stopped' if done == -1 else 'failed: see the messages below')
            self.progress.emit(self.title, 'done' if ok else 'failed', 1.0 if ok else -1, False)
            if ok: self.win.library.after_task()
    def start(self, title, args, after=None):
        if self.busy() or self.ext: QMessageBox.warning(self, 'Busy', 'A build is running: wait for it or stop it'); return
        self.live = None; self.title = title; self.head.setText(title); self.sub.setText('starting…'); self.tree.clear(); self.items = {}; self.log.clear(); self.after = after; self.buf = ''
        self.proc = QProcess(self); self.proc.setProgram(sys.executable); self.proc.setArguments([str(SCRIPT), *args, '--progress', 'json'])
        bid = uuid.uuid4().hex; self.ids.add(bid)
        env = QProcessEnvironment.systemEnvironment(); env.insert('PYTHONUTF8', '1'); env.insert('PYTHONUNBUFFERED', '1'); env.insert('NXNATIVE_BUILD_ID', bid); self.proc.setProcessEnvironment(env)
        self.proc.setProcessChannelMode(QProcess.MergedChannels); self.proc.readyRead.connect(self.read); self.proc.finished.connect(self.finished)
        self.proc.start(); self.t0 = time.time(); self.say(f'$ python {SCRIPT.name} ' + ' '.join(args), TEXT3)
        self.progress.emit(title, 'starting…', -1, True)
    def say(self, text, color=None):
        if color is None: color = ERR if '[RED]' in text else None
        self.log.appendHtml(f'<span style="color:{color}">{esc(text)}</span>' if color else esc(text))
    def read(self):
        if not self.proc: return
        self.buf += bytes(self.proc.readAll()).decode('utf-8', 'replace')
        *lines, self.buf = self.buf.split('\n')
        for line in lines: self.feed(line.rstrip('\r'))
    def feed(self, line):   # one line of the script's output (or of a journal): an event, or a plain message
        try: ev = json.loads(line) if line.startswith('{') else None
        except ValueError: ev = None
        if not ev:
            if line.strip(): self.say(line)
            return
        k = ev.get('ev')
        if k == 'say': self.say(ev.get('msg', '')); return
        if k not in ('stage', 'step', 'progress', 'stage_end'): return
        key = ev.get('k'); it = self.items.get(key)
        if it is None:
            it = QTreeWidgetItem([f'{key}/{ev.get("n", "?")}   {ev.get("name", "")}', '', '', '']); self.tree.addTopLevelItem(it); self.items[key] = it
            bar = QProgressBar(); bar.setRange(0, 0); bar.setTextVisible(False); self.tree.setItemWidget(it, 1, bar); it.t0 = ev.get('t') or time.time(); it.n = ev.get('n', '?'); it.name = ev.get('name', '')
        bar = self.tree.itemWidget(it, 1); frac = -1.0
        if ev.get('step'): it.setText(0, f'{key}/{it.n}   {it.name} · {ev["step"]}')
        if k == 'progress' and ev.get('total'):
            frac = (ev.get('done') or 0) / ev['total']; bar.setRange(0, 1000); bar.setValue(int(1000 * frac))
        it.setText(2, clock_text(time.time() - it.t0))
        detail = f'{it.name}' + (f' · {ev["step"]}' if ev.get('step') else '')
        if k == 'progress' and ev.get('total'): detail += (f' · {ev["note"]}' if ev.get('note') else '') + ' · ' + count_text(ev.get('done') or 0, ev['total'], ev.get('unit') or '')
        if ev.get('eta'): detail += f' · {int(ev["eta"])}s left'
        if k == 'stage_end':
            bar.setRange(0, 1); bar.setValue(1); it.setText(2, clock_text(ev.get('elapsed', 0))); it.setText(3, ev.get('summary', '')); it.setForeground(3, QColor(READY if ev.get('ok') else ERR))
            detail = f'{it.name} done'; frac = 1.0; QTimer.singleShot(0, self.win.library.reload)
        self.sub.setText(f'stage {key} of {it.n} · {detail}')
        self.live = None if k == 'stage_end' else (it, detail, frac if frac >= 0 else (self.live[2] if self.live and self.live[0] is it and k != 'step' else -1))
        self.progress.emit(self.title, detail, self.live[2] if self.live else frac, True)
    def tick(self):   # the running stage's clock, every second: a long step (adb copying the RomFS) sends no events
        if not self.live or not (self.busy() or self.ext): return
        it, detail, frac = self.live; t = clock_text(time.time() - it.t0)
        it.setText(2, t); self.progress.emit(self.title, f'{detail} · {t}', frac, True)
    def finished(self, code, _status):
        self.read()
        ok = code == 0; self.live = None; self.say(f'— finished in {time.time() - self.t0:.0f} s, exit {code}', READY if ok else ERR)
        self.sub.setText('done' if ok else 'failed: see the messages below')
        self.progress.emit(self.title, 'done' if ok else 'failed', 1.0 if ok else -1, False)
        after, self.after = self.after, None
        if ok and after: QTimer.singleShot(200, after)
        if not ok: self.win.toast(f'{self.title} failed: see Builds')
    def stop(self):
        if self.ext:   # a console build: its process tree
            if QMessageBox.question(self, 'Stop', f'Stop {self.title}, started from the console?') != QMessageBox.Yes: return
            if os.name == 'nt': subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.ext['pid'])], capture_output=True, creationflags=NOWINDOW)
            else: os.kill(self.ext['pid'], 15)
            return
        if not self.busy() or not self.proc: return
        if os.name == 'nt': subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.proc.processId())], capture_output=True, creationflags=NOWINDOW)   # its tools too
        else: self.proc.kill()


class BuildBar(QWidget):   # the running build at the bottom of every page
    def __init__(self, win):
        super().__init__(); self.setObjectName('buildbar'); self.setAttribute(Qt.WA_StyledBackground, True); self.win = win
        lay = QHBoxLayout(self); lay.setContentsMargins(28, 16, 28, 16); lay.setSpacing(20)
        ic = QLabel(); ic.setPixmap(icon('tools', TEXT2, 26).pixmap(26, 26)); lay.addWidget(ic)
        col = QVBoxLayout(); col.setSpacing(2); self.title = label(''); self.title.setStyleSheet('font-size: 15px; font-weight: 700'); self.detail = label('', 'meta')
        col.addWidget(self.title); col.addWidget(self.detail); lay.addLayout(col, 3)
        self.bar = QProgressBar(); self.bar.setRange(0, 1000); self.bar.setTextVisible(False); lay.addWidget(self.bar, 4)
        self.pct = label(''); self.pct.setStyleSheet(f'font-size: 15px; font-weight: 600; color: {TEXT2}'); self.pct.setMinimumWidth(48); lay.addWidget(self.pct)
        self.view = QPushButton('View build'); self.view.clicked.connect(lambda: win.go('builds')); lay.addWidget(self.view)
        self.stop = QPushButton('Stop'); self.stop.setObjectName('stop'); self.stop.clicked.connect(win.stop_build); lay.addWidget(self.stop)
        self.hide(); self.hide_timer = QTimer(self); self.hide_timer.setSingleShot(True); self.hide_timer.timeout.connect(self.hide)
    def update_from(self, title, detail, frac, running):
        self.show(); self.hide_timer.stop()
        self.title.setText(f'{title} in progress' if running else title + (' — done' if frac >= 0 else ' — failed'))
        self.detail.setText(detail)
        if frac < 0: self.bar.setRange(0, 0 if running else 1000); self.pct.setText('')
        else: self.bar.setRange(0, 1000); self.bar.setValue(int(frac * 1000)); self.pct.setText(f'{frac * 100:.0f}%')
        self.stop.setVisible(running)
        if not running: self.hide_timer.start(8000)


# ---------------- Phones ----------------
class PhonesPage(QWidget):
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page')
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        lay.addWidget(label('Phones', 'h1'))
        lay.addWidget(label('Over Wi-Fi: on the phone, Developer options → Wireless debugging. "Pair device with pairing code" gives an address and a code for Pair; '
                            'the address on the Wireless debugging screen itself is the one to Connect to (its port changes when wireless debugging restarts).', 'muted', wrap=True))
        self.table = QTableWidget(0, 5); self.table.setHorizontalHeaderLabels(['Device', 'Model', 'Android', 'Connection', 'State'])
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch); self.table.verticalHeader().hide(); self.table.setShowGrid(False)
        self.table.setSelectionBehavior(QTableWidget.SelectRows); self.table.setEditTriggers(QTableWidget.NoEditTriggers); self.table.itemSelectionChanged.connect(self.selected)
        self.table.setMaximumHeight(200); lay.addWidget(self.table)
        row = QHBoxLayout()
        for text, fn in (('Pair over Wi-Fi…', self.pair), ('Connect…', self.connect), ('Discover', self.discover), ('Disconnect', self.disconnect)):
            b = QPushButton(text); b.clicked.connect(fn); row.addWidget(b)
        row.addStretch(); lay.addLayout(row)
        lay.addSpacing(10); self.ph = label('The picked phone', 'h2'); lay.addWidget(self.ph); self.app = label('—', 'meta', wrap=True); lay.addWidget(self.app)
        self.games = QListWidget(); lay.addWidget(self.games, 1)
        row = QHBoxLayout()
        for text, fn, name in (('Update the app', self.update_app, None), ('Back up saves to this computer', self.backup, None), ('Move the old one-game install…', self.migrate, 'ghost')):
            b = QPushButton(text); b.clicked.connect(fn); row.addWidget(b)
            if name: b.setObjectName(name)
        row.addStretch(); lay.addLayout(row)
    def fill(self, devs):
        self.table.setRowCount(len(devs))
        for i, d in enumerate(devs):
            for j, v in enumerate((d['serial'], d.get('model', ''), d.get('android', ''), 'Wi-Fi' if ':' in d['serial'] or '._adb-tls' in d['serial'] else 'USB', d['state'])):
                it = QTableWidgetItem(v)
                if j == 4: it.setForeground(QColor(READY if d['state'] == 'device' else ERR))
                self.table.setItem(i, j, it)
    def selected(self):
        r = self.table.currentRow()
        if r >= 0 and self.table.item(r, 0): self.win.set_serial(self.table.item(r, 0).text())
    def show_phone(self, pkg, games, legacy):
        self.ph.setText(self.win.phone_name() or 'No phone'); self.games.clear()
        self.app.setText(('The nxnative app · ' + ' · '.join(x.strip() for x in pkg.splitlines())) if 'lastUpdateTime' in pkg else 'The nxnative app is not installed')
        for gid, meta in games.items():
            it = QListWidgetItem(f'{meta.get("name") or gid}     {gid} · {meta.get("display") or meta.get("version") or "?"}'); it.setData(Qt.UserRole, gid); self.games.addItem(it)
        if legacy: self.games.addItem('A one-game install (files/game, before the launcher): Move the old one-game install puts it in the launcher, its save copied over')
    def pair(self):
        addr, ok = QInputDialog.getText(self, 'Pair over Wi-Fi', 'Address and port from "Pair device with pairing code" (e.g. 192.168.1.125:37123):')
        if not ok or not addr: return
        code, ok = QInputDialog.getText(self, 'Pair over Wi-Fi', 'Pairing code:')
        if not ok or not code: return
        bg(lambda: adb_run(None, 'pair', addr.strip(), code.strip(), timeout=30), lambda r: (self.win.toast(r[1] if not isinstance(r, Exception) else str(r)), self.connect()))
    def connect(self):
        addr, ok = QInputDialog.getText(self, 'Connect', 'Address and port from the Wireless debugging screen (e.g. 192.168.1.125:39973):', text=self.win.st.value('wifi', ''))
        if not ok or not addr: return
        self.win.st.setValue('wifi', addr.strip())
        bg(lambda: adb_run(None, 'connect', addr.strip(), timeout=30), lambda r: (self.win.toast(r[1] if not isinstance(r, Exception) else str(r)), self.win.refresh_devices()))
    def discover(self):
        def got(r):
            text = r[1] if not isinstance(r, Exception) else str(r)
            found = re.findall(r'_adb-tls-connect\._tcp\S*\s+(\d+\.\d+\.\d+\.\d+:\d+)', text)
            if found and QMessageBox.question(self, 'Discover', f'Phones offering wireless debugging:\n{text}\n\nConnect to {found[0]}?') == QMessageBox.Yes:
                bg(lambda: adb_run(None, 'connect', found[0], timeout=30), lambda _: self.win.refresh_devices())
            elif not found: QMessageBox.information(self, 'Discover', text or 'Nothing found: pair the phone first, and keep its Wireless debugging screen open')
        bg(lambda: adb_run(None, 'mdns', 'services'), got)
    def disconnect(self):
        s = self.win.serial()
        if s and ':' in s: bg(lambda: adb_run(None, 'disconnect', s), lambda _: self.win.refresh_devices())
    def update_app(self):
        s = self.win.serial()
        if s: self.win.builds.start('Update the app', ['--apk-only', '--serial', s], after=self.win.refresh_phone)
    def migrate(self):
        s = self.win.serial()
        if not s: return
        lib = ba.library()
        items = [f'{m.get("name") or gid} ({gid})' for gid, (_, m) in lib.items()]
        if not items: QMessageBox.information(self, 'Move', 'Build the game first, so the app gets its host'); return
        pick, ok = QInputDialog.getItem(self, 'Move the old one-game install', 'The game it holds (its save is copied to that game; the old save stays as it was):', items, 0, False)
        if not ok: return
        gid = pick.rsplit('(', 1)[-1].rstrip(')')
        if QMessageBox.question(self, 'Move', 'First the save backups on the phone are copied to this computer, then the game folder moves and the app is updated; '
                                'the app copies the save when it next opens. Go on?') != QMessageBox.Yes: return
        self.backup(then=lambda: self.win.builds.start('Move the old install', ['--migrate', '--title', gid, '--serial', s],
                                                       after=lambda: self.win.builds.start('Update the app', ['--apk-only', '--serial', s], after=self.win.refresh_phone)))
    def backup(self, then=None):
        s = self.win.serial()
        if not s: return
        dst = ba.data_dir() / 'backups' / re.sub(r'[^\w.-]', '_', s) / time.strftime('%Y%m%d-%H%M%S')
        def run():
            ok, out = adb_run(s, 'shell', f'ls {REMOTE}/backups')
            if not ok or not out.strip(): return None, 'no backups on the phone yet: open the app and use Back up saves in its game list'
            dst.mkdir(parents=True, exist_ok=True)
            return adb_run(s, 'pull', f'{REMOTE}/backups', str(dst), timeout=600)
        def got(r):
            if isinstance(r, Exception): QMessageBox.warning(self, 'Backup', str(r)); return
            ok, out = r
            if ok: self.win.toast(f'The phone\'s save backups are in {dst}')
            elif not then: QMessageBox.warning(self, 'Backup', out)
            if then and ok: then()
            elif then and QMessageBox.question(self, 'Backup', f'No backup was copied ({out}). Go on anyway? The old save stays on the phone either way.') == QMessageBox.Yes: then()
        bg(run, got)
    def remove_game(self, gid, name):
        s = self.win.serial()
        if not s or QMessageBox.question(self, 'Remove', f'Delete {name} from {self.win.phone_name()}? Its save stays in the app.') != QMessageBox.Yes: return
        bg(lambda: adb_run(s, 'shell', 'rm', '-rf', f'{REMOTE}/games/{gid}'), lambda _: self.win.refresh_phone())


# ---------------- Performance ----------------
class Chart(QWidget):   # frame time history (ms), with the 16.7 and 33.3 ms lines
    def __init__(self): super().__init__(); self.vals = []; self.setMinimumHeight(150)
    def add(self, v): self.vals = (self.vals + [v])[-120:]; self.update()
    def paintEvent(self, _):
        p = QPainter(self); p.setRenderHint(QPainter.Antialiasing); w, h = self.width(), self.height(); top = max([50.0] + self.vals) * 1.1
        path = QPainterPath(); path.addRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), 12, 12); p.fillPath(path, QColor(PANEL)); p.setPen(QPen(QColor(255, 255, 255, 20))); p.drawPath(path)
        f = QFont(); f.setFamilies([FONT, 'Segoe UI']); f.setPixelSize(11); p.setFont(f)
        for ms, c in ((16.7, READY), (33.3, BUILD)):
            y = h - ms / top * h; col = QColor(c); col.setAlpha(110); p.setPen(QPen(col, 1, Qt.DashLine)); p.drawLine(12, int(y), w - 12, int(y)); p.drawText(16, int(y) - 4, f'{ms} ms')
        if len(self.vals) > 1:
            p.setPen(QPen(QColor(TEXT), 2)); step = (w - 24) / (len(self.vals) - 1)
            for i in range(1, len(self.vals)): p.drawLine(int(12 + (i - 1) * step), int(h - self.vals[i - 1] / top * h), int(12 + i * step), int(h - self.vals[i] / top * h))
        else: p.setPen(QColor(TEXT3)); p.drawText(self.rect(), Qt.AlignCenter, 'Watch the log while a game runs: its frame time shows here every ~10 s')


class PerformancePage(QWidget):
    PATS = {'frame': re.compile(r'timing up to frame \d+: frame time p50 ([\d.]+) / p95 ([\d.]+)'), 'gpu': re.compile(r'GPU busy ([\d.]+) ms'),
            'cpu': re.compile(r'translation up to .*CPU ([\d.]+) ms, (\d+) draws'), 'speed': re.compile(r'audio clock rate ([\d.]+)'),
            'wait': re.compile(r'SyncWait blocked \d+ times: mean ([\d.]+) ms'), 'pass': re.compile(r'\[prof\]\s+(\d+) us  pass #\d+  (\d+)x(\d+)  colour \d+ \(vk\d+\) depth \d  draws (\d+)')}

    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page'); self.proc = None; self.buf = ''; self.passes = []
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        head = QHBoxLayout(); head.addWidget(label('Performance', 'h1')); head.addStretch()
        self.b_run = QPushButton('Watch the log'); self.b_run.clicked.connect(self.toggle); head.addWidget(self.b_run)
        for text, fn in (('Profile a frame', self.profile), ('Thermal', self.thermal), ('Open the app', self.launch), ('Stop the app', self.stop_app), ('Save log.txt…', self.save)):
            b = QPushButton(text); b.setObjectName('ghost'); b.clicked.connect(fn); head.addWidget(b)
        lay.addLayout(head)
        tiles = QGridLayout(); tiles.setSpacing(12); self.tiles = {}
        for i, (k, t) in enumerate((('fps', 'FRAMES PER SECOND'), ('frame', 'FRAME TIME P50 / P95'), ('gpu', 'GPU BUSY PER FRAME'), ('cpu', 'REPLAY CPU PER FRAME'),
                                    ('speed', 'GAME SPEED'), ('wait', 'WAIT FOR THE GPU'))):
            f = QFrame(); f.setObjectName('tile'); fl = QVBoxLayout(f); fl.setContentsMargins(16, 14, 16, 14)
            v = label('—', 'tilevalue'); fl.addWidget(label(t, 'tilelabel')); fl.addWidget(v); self.tiles[k] = v; tiles.addWidget(f, 0, i)
        lay.addLayout(tiles); self.chart = Chart(); lay.addWidget(self.chart)
        split = QSplitter(Qt.Horizontal)
        left = QWidget(); ll = QVBoxLayout(left); ll.setContentsMargins(0, 0, 0, 0)
        self.filter = QLineEdit(); self.filter.setPlaceholderText('Filter the log (e.g. RED)'); ll.addWidget(self.filter)
        self.log = QPlainTextEdit(); self.log.setReadOnly(True); self.log.setMaximumBlockCount(5000); self.log.setFont(QFont('Cascadia Mono', 9)); ll.addWidget(self.log)
        split.addWidget(left)
        self.prof = QTableWidget(0, 3); self.prof.setHorizontalHeaderLabels(['GPU passes of a frame', 'ms', 'share']); self.prof.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.prof.verticalHeader().hide(); self.prof.setShowGrid(False); split.addWidget(self.prof); split.setSizes([700, 380])
        lay.addWidget(split, 1)
    def toggle(self):
        if self.proc and self.proc.state() != QProcess.NotRunning: self.proc.kill(); self.b_run.setText('Watch the log'); return
        s = self.win.serial()
        if not s: self.win.toast('Connect a phone first (Phones)'); return
        self.proc = QProcess(self); self.proc.setProgram('adb'); self.proc.setArguments(['-s', s, 'logcat', '-T', '200', '-s', 'nxnative'])
        self.proc.readyReadStandardOutput.connect(self.read); self.proc.start(); self.b_run.setText('Stop watching')
    def read(self):
        if not self.proc: return
        self.buf += bytes(self.proc.readAllStandardOutput()).decode('utf-8', 'replace')
        *lines, self.buf = self.buf.split('\n'); flt = self.filter.text().strip()
        for line in lines:
            line = line.rstrip('\r'); self.parse(line)
            if not flt or flt.lower() in line.lower():
                self.log.appendHtml(f'<span style="color:{ERR}">{esc(line)}</span>' if '[RED]' in line or 'CRASH' in line else esc(line))
    def parse(self, line):
        P = self.PATS
        if m := P['frame'].search(line):
            p50, p95 = float(m[1]), float(m[2]); self.tiles['frame'].setText(f'{p50:.1f} / {p95:.1f}'); self.tiles['fps'].setText(f'{1000 / p50:.0f}' if p50 else '—'); self.chart.add(p50)
        if m := P['gpu'].search(line): self.tiles['gpu'].setText(f'{float(m[1]):.1f} ms')
        if m := P['cpu'].search(line): self.tiles['cpu'].setText(f'{float(m[1]):.1f} ms')
        if m := P['speed'].search(line): v = float(m[1]); self.tiles['speed'].setText(f'{v * 100:.0f}%'); self.tiles['speed'].setStyleSheet(f'color: {READY if v > 0.97 else BUILD}')
        if m := P['wait'].search(line): self.tiles['wait'].setText(f'{float(m[1]):.1f} ms')
        if m := P['pass'].search(line): self.passes.append((int(m[1]), int(m[2]), int(m[3]), int(m[4]))); QTimer.singleShot(500, self.show_passes)
    def show_passes(self):   # the profiled frame's passes, grouped: without draws (clears), full size, small (bloom, blur, shadows), the rest (3D at the render size)
        if not self.passes: return
        groups = {}
        for us, w, h, draws in self.passes:
            k = 'Without draws (clears)' if draws == 0 else 'Full size (720p / 1080p)' if w >= 1280 else 'Small (bloom, blur, shadows)' if w < 400 else '3D scene and the rest'
            groups[k] = groups.get(k, 0) + us
        total = sum(groups.values()) or 1; self.prof.setRowCount(len(groups) + 1)
        for i, (k, us) in enumerate(sorted(groups.items(), key=lambda x: -x[1])):
            for j, v in enumerate((k, f'{us / 1000:.1f}', f'{100 * us / total:.0f}%')): self.prof.setItem(i, j, QTableWidgetItem(v))
        for j, v in enumerate((f'{len(self.passes)} passes', f'{total / 1000:.1f}', '')): self.prof.setItem(len(groups), j, QTableWidgetItem(v))
        self.passes = []
    def profile(self):
        s = self.win.serial()
        if not s: return
        if not (self.proc and self.proc.state() != QProcess.NotRunning): self.toggle()
        self.passes = []; bg(lambda: adb_run(s, 'shell', f'touch {REMOTE}/prof_now'), lambda r: self.win.toast('A frame is profiled within ~1 s while a game runs'))
    def thermal(self):
        s = self.win.serial()
        if s: bg(lambda: adb_run(s, 'shell', 'dumpsys thermalservice | grep -E "Thermal Status|cpufreq|devfreq|Temperature\\{" | head -20; dumpsys battery | grep -E "temperature|powered"'),
                 lambda r: QMessageBox.information(self, 'Thermal', (r[1] if not isinstance(r, Exception) else str(r)) + '\n\nA cpufreq value above 0: the phone limits those cores because of heat.'))
    def launch(self):
        s = self.win.serial()
        if s: bg(lambda: adb_run(s, 'shell', 'am', 'start', '-n', f'{PKG}/.Launcher'), lambda _: None)
    def stop_app(self):
        s = self.win.serial()
        if s: bg(lambda: adb_run(s, 'shell', 'am', 'force-stop', PKG), lambda _: None)
    def save(self):
        s = self.win.serial()
        if not s: return
        f, _ = QFileDialog.getSaveFileName(self, 'Save the app log', 'log.txt', 'Text (*.txt)')
        if f: bg(lambda: adb_run(s, 'pull', f'{REMOTE}/log.txt', f, timeout=60), lambda r: self.win.toast(r[1] if not isinstance(r, Exception) else str(r)))


# ---------------- Settings ----------------
class SettingsPage(QWidget):
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page')
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        lay.addWidget(label('Settings', 'h1'))
        keys = QHBoxLayout(); keys.addWidget(label('Your prod.keys', 'meta')); self.keys = QLineEdit(win.st.value('keys', '')); self.keys.setPlaceholderText('the keys dumped from your Switch')
        self.keys.editingFinished.connect(lambda: win.st.setValue('keys', self.keys.text().strip())); keys.addWidget(self.keys, 1)
        b = QPushButton('Choose…'); b.clicked.connect(self.pick_keys); keys.addWidget(b); lay.addLayout(keys)
        lay.addSpacing(8)
        head = QHBoxLayout(); head.addWidget(label('Requirements', 'h2')); head.addStretch(); b = QPushButton('Check again'); b.setObjectName('ghost'); b.clicked.connect(lambda *_: self.run()); head.addWidget(b)
        lay.addLayout(head)
        lay.addWidget(label(f'The build runs with {sys.executable}: install missing Python modules into it.', 'muted', wrap=True))
        self.table = QTableWidget(0, 3); self.table.setHorizontalHeaderLabels(['', 'What', 'Detail / how to get it']); self.table.verticalHeader().hide(); self.table.setShowGrid(False)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch); self.table.setColumnWidth(0, 36); self.table.setColumnWidth(1, 220); self.table.setWordWrap(True)
        lay.addWidget(self.table, 1)
    def pick_keys(self):
        f, _ = QFileDialog.getOpenFileName(self, 'Your prod.keys', '', 'Keys (*.keys);;All files (*)')
        if f: self.keys.setText(f); self.win.st.setValue('keys', f)
    def run(self, then=None):
        def got(rows):
            if isinstance(rows, Exception): rows = [{'name': 'check', 'ok': False, 'detail': str(rows), 'fix': ''}]
            self.table.setRowCount(len(rows))
            for i, r in enumerate(rows):
                mark = QTableWidgetItem('●'); mark.setForeground(QColor(READY if r['ok'] else ERR)); mark.setTextAlignment(Qt.AlignCenter); self.table.setItem(i, 0, mark)
                self.table.setItem(i, 1, QTableWidgetItem(r['name'])); self.table.setItem(i, 2, QTableWidgetItem(r['detail'] + (f'\n→ {r["fix"]}' if r['fix'] else '')))
            self.table.resizeRowsToContents()
            if then: then(rows)
        bg(lambda: self.win.script_json('--check'), got)


# ---------------- The window ----------------
class Window(QMainWindow):
    def __init__(self):
        super().__init__(); self.setWindowTitle('nxnative'); self.setMinimumSize(1000, 620)
        a = QApplication.primaryScreen().availableGeometry(); self.resize(min(1440, a.width() - 40), min(900, a.height() - 40))
        self.st = QSettings('nxnative', 'android'); self.devs = []; self.phone_games = {}; self.phone_legacy = False; self.models = {}
        central = QWidget(); central.setObjectName('page'); root = QVBoxLayout(central); root.setContentsMargins(0, 0, 0, 0); root.setSpacing(0); self.setCentralWidget(central)
        # the top bar: the wordmark, the pages, the phone, search, + Build game
        top = QWidget(); top.setObjectName('topbar'); top.setAttribute(Qt.WA_StyledBackground, True); tl = QHBoxLayout(top); tl.setContentsMargins(28, 14, 28, 14); tl.setSpacing(6)
        tl.addWidget(label('nxnative', 'wordmark'))
        self.navs = {}; group = QButtonGroup(self); group.setExclusive(True)
        for key, text, ic in (('library', 'Library', 'library'), ('builds', 'Builds', 'builds'), ('phones', 'Phones', 'phone'), ('perf', 'Performance', 'perf'), ('settings', 'Settings', 'settings')):
            b = QToolButton(); b.setObjectName('nav'); b.setText(f' {text}'); b.setIcon(icon(ic, TEXT2, 18)); b.setIconSize(QSize(18, 18)); b.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
            b.setCheckable(True); b.setCursor(Qt.PointingHandCursor); b.clicked.connect(lambda _=False, k=key: self.go(k)); group.addButton(b); tl.addWidget(b); self.navs[key] = b
        tl.addStretch()
        self.phone = QComboBox(); self.phone.setFixedWidth(250); self.phone.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon); self.phone.setMinimumContentsLength(18); self.phone.setToolTip('The phone builds install to'); self.phone.currentIndexChanged.connect(self.phone_changed)
        tl.addWidget(self.phone); tl.addSpacing(8)
        self.search = QLineEdit(); self.search.setPlaceholderText('Search your games'); self.search.setFixedWidth(220); self.search.hide()
        self.search.textChanged.connect(self.filter_games)
        sb = QToolButton(); sb.setObjectName('nav'); sb.setIcon(icon('search', TEXT, 20)); sb.setIconSize(QSize(20, 20)); sb.setCursor(Qt.PointingHandCursor); sb.clicked.connect(self.toggle_search)
        tl.addWidget(self.search); tl.addWidget(sb); tl.addSpacing(8)
        add = QPushButton('  Build game'); add.setObjectName('ghost'); add.setIcon(icon('plus', TEXT2, 16)); add.setCursor(Qt.PointingHandCursor); add.clicked.connect(self.add_game); tl.addWidget(add)
        root.addWidget(top)
        sep = QFrame(); sep.setFixedHeight(1); sep.setStyleSheet(f'background: {LINE}'); root.addWidget(sep)
        self.pages = QStackedWidget(); root.addWidget(self.pages, 1)
        self.builds = BuildsPage(self); self.phones = PhonesPage(self); self.perf = PerformancePage(self); self.settings = SettingsPage(self); self.library = LibraryPage(self)
        self.page_of = {'library': self.library, 'builds': self.builds, 'phones': self.phones, 'perf': self.perf, 'settings': self.settings}
        for p in self.page_of.values(): self.pages.addWidget(p)
        self.buildbar = BuildBar(self); root.addWidget(self.buildbar); self.builds.progress.connect(self.buildbar.update_from)
        self.toastl = label('', wrap=True); self.toastl.setParent(central); self.toastl.setStyleSheet(f'background: {LIFT}; border: 1px solid {LINE}; border-radius: 10px; padding: 10px 16px; font-size: 13px')
        self.toastl.hide(); self.toast_t = QTimer(self); self.toast_t.setSingleShot(True); self.toast_t.timeout.connect(self.toastl.hide)
        self.go('library')
        self.timer = QTimer(self); self.timer.timeout.connect(self.refresh_devices); self.timer.start(5000); self.refresh_devices()
        self.settings.run(then=lambda rows: self.go('settings') if any(not r['ok'] for r in rows) else None)
        if os.name == 'nt': self.dark_title()

    def dark_title(self):   # Windows' own title bar in its dark mode, to match the room
        try:
            import ctypes; v = ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(int(self.winId()), 20, ctypes.byref(v), ctypes.sizeof(v))  # pyright: ignore[reportAttributeAccessIssue]   (Windows only) DWMWA_USE_IMMERSIVE_DARK_MODE
        except Exception: pass
    def go(self, key):
        page = self.page_of.get(key, key) if isinstance(key, str) else key
        self.pages.setCurrentWidget(page)
        for k, b in self.navs.items(): b.setChecked(self.page_of[k] is page); b.setIcon(icon({'library': 'library', 'builds': 'builds', 'phones': 'phone', 'perf': 'perf', 'settings': 'settings'}[k], TEXT if self.page_of[k] is page else TEXT2, 18))
    def toast(self, text):
        self.toastl.setText(text); self.toastl.setMaximumWidth(560); self.toastl.adjustSize()
        self.toastl.move(self.width() - self.toastl.width() - 28, 84); self.toastl.raise_(); self.toastl.show(); self.toast_t.start(5000)
    def toggle_search(self):
        on = not self.search.isVisible(); self.search.setVisible(on)
        if on: self.search.setFocus(); self.go('library')
        else: self.search.clear()
    def filter_games(self, t): self.library.filter = t; self.library.render()
    def add_game(self):
        d = QFileDialog.getExistingDirectory(self, 'The folder with your game\'s NSP / XCI files')
        if not d: return
        fs = [f for f in (self.st.value('folders', []) or []) if f]
        if d not in fs: self.st.setValue('folders', fs + [d])
        self.go('library'); self.library.reload(); self.toast('Reading the game… then Build and install puts it on your phone')
    def stop_build(self): self.builds.stop()
    def script_json(self, *args):
        r = subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True, encoding='utf-8', errors='replace', creationflags=NOWINDOW, env=dict(os.environ, PYTHONUTF8='1'))
        if r.returncode: raise RuntimeError((r.stderr or r.stdout).strip()[-500:])
        return json.loads(r.stdout)
    def serial(self): return self.phone.currentData() or None
    def phone_name(self): return self.models.get(self.serial()) or self.serial()
    def set_serial(self, s):
        i = self.phone.findData(s)
        if i >= 0: self.phone.setCurrentIndex(i)
    def phone_changed(self, _):
        s = self.serial()
        if s: self.st.setValue('serial', s)
        self.refresh_phone()
    def refresh_phone(self):   # the app and the games on the picked phone: the Library's statuses and the Phones page
        s = self.serial()
        if not s: self.phone_games = {}; self.library.refresh_status(); self.phones.show_phone('', {}, False); return
        def look():
            _, pkg = adb_run(s, 'shell', f'dumpsys package {PKG} | grep -E "lastUpdateTime|versionName" | head -2')
            _, ls = adb_run(s, 'shell', f'for d in {REMOTE}/games/*/; do [ -f "$d/code/main.fixups.tsv" ] || continue; echo "@$d"; cat "$d/game.json" 2>/dev/null | tr -d "\\n"; echo; done; '
                                        f'[ -f {REMOTE}/game/code/main.fixups.tsv ] && echo LEGACY')
            games, legacy, cur = {}, False, None
            for l in ls.splitlines():
                if l == 'LEGACY': legacy = True
                elif l.startswith('@'): cur = l[1:].rstrip('/').rsplit('/', 1)[-1].upper(); games[cur] = {}
                elif cur and l.strip().startswith('{'):
                    try: games[cur] = json.loads(l)
                    except ValueError: pass
            return pkg, games, legacy
        def got(r):
            if isinstance(r, Exception): return
            pkg, games, legacy = r
            self.phone_games = games if 'lastUpdateTime' in pkg else {}; self.phone_legacy = legacy
            self.library.refresh_status(); self.phones.show_phone(pkg, games, legacy)
        bg(look, got)
    def refresh_devices(self):
        def look():
            ok, out = adb_run(None, 'devices', '-l', timeout=10)
            devs = []
            for line in out.splitlines()[1:] if ok else []:
                c = line.split()
                if len(c) < 2: continue
                d = {'serial': c[0], 'state': c[1], 'model': next((x[6:] for x in c if x.startswith('model:')), '').replace('_', ' ')}
                if d['state'] == 'device':
                    d['android'] = adb_run(d['serial'], 'shell', 'getprop', 'ro.build.version.release', timeout=8)[1]
                    d['name'] = adb_run(d['serial'], 'shell', 'settings', 'get', 'global', 'device_name', timeout=8)[1] or d['model']
                devs.append(d)
            return devs if ok else out
        def got(r):
            if isinstance(r, (Exception, str)): return
            if [(d['serial'], d['state']) for d in r] == [(d['serial'], d['state']) for d in self.devs]: return
            self.devs = r; keep = self.serial() or self.st.value('serial', '')
            self.models = {d['serial']: d.get('name') or d.get('model') or d['serial'] for d in r}
            self.phone.blockSignals(True); self.phone.clear()
            for d in r:
                if d['state'] == 'device': self.phone.addItem(icon('phone', TEXT2, 16), f'{self.models[d["serial"]]}' + (f' · Android {d.get("android")}' if d.get('android') else ''), d['serial'])
            if not self.phone.count(): self.phone.addItem(icon('phone', TEXT3, 16), 'No phone connected', None)
            i = self.phone.findData(keep); self.phone.setCurrentIndex(i if i >= 0 else 0); self.phone.blockSignals(False)
            self.phones.fill(r); self.phone_changed(0)
        bg(look, got)


def main():
    app = QApplication(sys.argv); app.setApplicationName('nxnative'); app.setStyle('Fusion'); app.setStyleSheet(STYLE)
    w = Window(); w.show(); sys.exit(app.exec())


if __name__ == '__main__':
    main()
