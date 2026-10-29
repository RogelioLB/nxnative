// The app's Java side (front_android.c loads it through the activity's class loader and registers the natives): the virtual gamepad, the software
// keyboard's text box, the shake sensor.
// The virtual gamepad is dm/ios/Sources/DmGamepad.m on Android (its file header has the design; the constants, layouts and touch ownership are the same, in dp for points):
//   2D (simple), 3D (full, with D-pad, L3 / R3 and a right stick in the right third) and Tap (no controls: touches go to the game's touch screen; handheld mode only),
//   cycled by the layout button at the top centre and kept per app (each game is its own app). Touches no control takes go to the game's touch screen, normalized
//   on the centred 16:9 picture. The controls hide while a physical controller is connected (its keys reach the native input queue: this window is not focusable).
// NativeActivity owns its window's surface (no view of its own is drawn), so the gamepad is a PopupWindow over the whole activity.
// Shaking the phone shakes the controller (the accelerometer: gravity followed by a slow low-pass, a hit past DM_SHAKE_G, 1.5 G by default).
package org.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.TextView;
import java.util.concurrent.CountDownLatch;

public final class Gamepad extends View {
    static native void nativeTouchPad(long buttons, int lx, int ly, int rx, int ry);   // the controls' state (front_android.c merges it with a physical controller's)
    static native void nativeGameTouch(int[] ids, float[] xs, float[] ys, int n);      // the game's touch screen: dm_touch_set
    static native void nativeShake(boolean on);
    static native int nativeOpMode();          // hle_oe.c dm_opmode: 0 handheld
    static native int nativeDefaultLayout();   // the game profile's pad_layout (DM_GAME_PAD_LAYOUT)
    static native void nativeLog(String s);
    static native String nativeEnv(String name);   // the host's environment (env.txt is read after the VM starts: System.getenv does not see it)
    static native long nativeFrames();         // frames the game presented (front_android.c): the frame-rate counter

    static final int B_A = 0, B_B = 1, B_X = 2, B_Y = 3, B_LS = 4, B_RS = 5, B_L = 6, B_R = 7, B_ZL = 8, B_ZR = 9, B_PLUS = 10, B_MINUS = 11,
                     B_LEFT = 12, B_UP = 13, B_RIGHT = 14, B_DOWN = 15, B_N = 16;   // the bits of dm_input.h
    static final int C_NONE = 0, C_STICK = 1, C_DPAD = 2, C_FACE = 3, C_BTN = 4, C_SWAP = 5, C_GAME = 6, C_MENU = 7;
    // DmGamepad.m's layout constants (in u; x from the outer edge, the right half mirrors)
    static final float K_ZL_X = 44, K_ZL_Y = 40, K_ZL_R = 26, K_L_X = 44, K_L_Y = 108, K_L_R = 24, K_DROP_TIP = 1.45f, K_CORNER_IN = 0.5f,
        K_FACE_X = 81, K_PAD_Y = 82, K_FACE_GAP = 50, K_FACE_R = 27, K_PILL_DX = 52, K_PILL_Y = 22, K_PILL_W = 50, K_PILL_H = 28, K_SWAP_R = 15,
        K_STICK_X = 81, K_STICK_R = 77, K_KNOB_R = 30, K_STICK_GRAB = 1.4f, K_FULL = 0.8f, K_DEAD = 0.1f,
        K_LOW_X = 231, K_LOW_Y = 64, K_DPAD_R = 48, K_ARM_W = 17, K_ARM_IN = 11, K_DPAD_GRAB = 1.3f, K_RSTICK_R = 60, K_RSWIPE = 1f / 3, K_CLICK_X = 102, K_CLICK_R = 17;
    static final int[] K_BTNS = {B_ZL, B_L, B_ZR, B_R, B_MINUS, B_PLUS, B_LS, B_RS};
    static final String[] NAMES = {"A", "B", "X", "Y", "L3", "R3", "L", "R", "ZL", "ZR", "+", "−", null, null, null, null};

    final float dp; final SharedPreferences prefs;
    boolean full, tap, padOff;
    // touches by pointer id: owner (control | index << 8), the diamond keys each holds
    final long[] owner = new long[64]; final long[] faceOf = new long[64]; final boolean[] down = new boolean[64];
    final float[] px = new float[64], py = new float[64];
    int[] stick = {-1, -1}; float[] stickX = new float[2], stickY = new float[2]; float rcx, rcy;
    long faceBits, dpadBits, btnBits; boolean swapHeld, menuHeld, menuGo; int hapticSticks;
    final int[] game = new int[16];   // pointer id holding each finger slot of the game's touch screen, -1 free
    final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    static final int IDLE = Color.argb(36, 255, 255, 255), ON = Color.argb(115, 255, 255, 255), LINE = Color.argb(97, 255, 255, 255),
                     RING = Color.argb(15, 255, 255, 255), TEXT = Color.argb(153, 255, 255, 255), TEXT_ON = Color.argb(179, 0, 0, 0);
    float insL, insR, insT, insB;
    long lastBtn; int[] lastAx = new int[4];   // what was last sent (the self-check reads it)

    Gamepad(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
        prefs = c.getSharedPreferences("nxnative", Context.MODE_PRIVATE);
        java.util.Arrays.fill(game, -1);
        line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(1.5f * dp); line.setColor(LINE);
        fill.setStyle(Paint.Style.FILL);
        text.setTextAlign(Paint.Align.CENTER); text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        int l = prefs.getInt("DmPadLayout", nativeDefaultLayout());
        if (l == 2 && nativeOpMode() != 0) l = 0;
        full = l == 1; tap = l == 2;
        setHapticFeedbackEnabled(true);
    }

    // ---- Layout (px; the safe area from the window's cutout insets) ----
    @Override public WindowInsets onApplyWindowInsets(WindowInsets in) {
        if (android.os.Build.VERSION.SDK_INT >= 28 && in.getDisplayCutout() != null) {
            android.view.DisplayCutout c = in.getDisplayCutout();
            insL = c.getSafeInsetLeft(); insR = c.getSafeInsetRight(); insT = c.getSafeInsetTop(); insB = c.getSafeInsetBottom();
        }
        invalidate(); return in;
    }
    float u() { return dp * Math.min(getHeight() / dp / 430f, 1.3f); }
    float safeL() { return insL; } float safeR() { return getWidth() - insR; } float safeT() { return insT; }
    float cornerL() { return insL * K_CORNER_IN; } float cornerR() { return getWidth() - insR * K_CORNER_IN; } float cornerB() { return getHeight() - insB; }
    float faceCx() { return cornerR() - K_FACE_X * u(); } float faceCy() { return cornerB() - K_PAD_Y * u(); }
    float faceGap() { return K_FACE_GAP * u(); } float faceR() { return K_FACE_R * u(); }
    float faceX(int b) { return b == B_A ? faceCx() + faceGap() : b == B_Y ? faceCx() - faceGap() : faceCx(); }
    float faceY(int b) { return b == B_B ? faceCy() + faceGap() : b == B_X ? faceCy() - faceGap() : faceCy(); }
    float homeX() { return cornerL() + K_STICK_X * u(); } float homeY() { return cornerB() - K_PAD_Y * u(); }
    float stickCx(int i) { return i == 1 ? rcx : homeX(); } float stickCy(int i) { return i == 1 ? rcy : homeY(); }
    float stickR(int i) { return (i == 1 ? K_RSTICK_R : K_STICK_R) * u(); }
    boolean inRightArea(float x) { return x >= getWidth() - getWidth() * K_RSWIPE; }
    float dpadCx() { return cornerL() + K_LOW_X * u(); } float dpadCy() { return cornerB() - K_LOW_Y * u(); } float dpadR() { return K_DPAD_R * u(); }
    static boolean isDrop(int b) { return b == B_ZL || b == B_L || b == B_ZR || b == B_R; }
    static boolean isClick(int b) { return b == B_LS || b == B_RS; }
    static boolean fullOnly(int b) { return b == B_LS || b == B_RS || b >= B_LEFT; }
    float dropCx(int b) { boolean z = b == B_ZL || b == B_ZR, r = b == B_ZR || b == B_R; float dx = (z ? K_ZL_X : K_L_X) * u(); return r ? safeR() - dx : safeL() + dx; }
    float dropCy(int b) { boolean z = b == B_ZL || b == B_ZR; return safeT() + (z ? K_ZL_Y : K_L_Y) * u(); }
    float dropR(int b) { return (b == B_ZL || b == B_ZR ? K_ZL_R : K_L_R) * u(); }
    float clickCx(int b) { return b == B_RS ? safeR() - K_CLICK_X * u() : safeL() + K_CLICK_X * u(); } float clickCy() { return safeT() + K_ZL_Y * u(); }
    RectF pill(int b) { float w = K_PILL_W * u(), h = K_PILL_H * u(), cx = (safeL() + safeR()) / 2 + (b == B_PLUS ? K_PILL_DX : -K_PILL_DX) * u(), cy = safeT() + K_PILL_Y * u();
        return new RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2); }
    float swapCx() { return (safeL() + safeR()) / 2; } float swapCy() { return safeT() + K_PILL_Y * u(); }
    // the menu button (back to the launcher), left of −: past the − pill's grown hit area, so the two never share a touch
    float menuCx() { return swapCx() - (K_PILL_DX + K_PILL_W / 2 + 14 + K_SWAP_R) * u(); } float menuCy() { return swapCy(); }
    static float dist(float x0, float y0, float x1, float y1) { return (float)Math.hypot(x0 - x1, y0 - y1); }
    int btnAt(float x, float y) {
        for (int b : K_BTNS) {
            if (fullOnly(b) && !full) continue;
            boolean hit = isDrop(b) ? dist(x, y, dropCx(b), dropCy(b)) <= dropR(b) * 1.35f
                        : isClick(b) ? dist(x, y, clickCx(b), clickCy()) <= K_CLICK_R * u() * 1.35f
                        : grow(pill(b), 8 * u()).contains(x, y);
            if (hit) return b;
        }
        return -1;
    }
    static RectF grow(RectF r, float d) { return new RectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }
    boolean inSwap(float x, float y) { return dist(x, y, swapCx(), swapCy()) <= K_SWAP_R * u() * 1.6f; }
    boolean inMenu(float x, float y) { return dist(x, y, menuCx(), menuCy()) <= K_SWAP_R * u() * 1.4f; }
    long faceAt(float x, float y) { long m = 0; float r = faceR() * 1.25f; for (int b = B_A; b <= B_Y; b++) if (dist(x, y, faceX(b), faceY(b)) <= r) m |= 1L << b; return m; }
    long dpadAt(float x, float y) {
        float dx = x - dpadCx(), dy = y - dpadCy();
        if (Math.hypot(dx, dy) < dpadR() * 0.25) return 0;
        double a = Math.toDegrees(Math.atan2(-dy, dx)); long m = 0;
        if (a > -67.5 && a < 67.5) m |= 1L << B_RIGHT;
        if (a > 22.5 && a < 157.5) m |= 1L << B_UP;
        if (a > 112.5 || a < -112.5) m |= 1L << B_LEFT;
        if (a < -22.5 && a > -157.5) m |= 1L << B_DOWN;
        return m;
    }
    float[] knob(int i) {   // the knob's offset from its stick's centre, clamped to the radius
        if (stick[i] < 0) return new float[]{0, 0};
        float dx = stickX[i] - stickCx(i), dy = stickY[i] - stickCy(i), d = (float)Math.hypot(dx, dy), r = stickR(i);
        if (d > r) { dx *= r / d; dy *= r / d; }
        return new float[]{dx, dy};
    }

    // ---- Touch ----
    void recompute() {
        long face = 0, btn = 0, dpad = 0; boolean swap = false, menu = false;
        for (int p = 0; p < 64; p++) if (down[p]) {
            int c = (int)(owner[p] & 0xff);
            if (c == C_FACE) face |= faceOf[p];
            else if (c == C_BTN) btn |= 1L << (owner[p] >> 8);
            else if (c == C_DPAD) dpad |= dpadAt(px[p], py[p]);
            else if (c == C_SWAP) swap = true;
            else if (c == C_MENU) menu = true;
        }
        int[] ax = new int[4];
        for (int i = 0; i < 2; i++) {
            float[] o = knob(i); float r = stickR(i), d = (float)Math.hypot(o[0], o[1]), dz = r * K_DEAD;
            if (d <= dz) continue;
            float m = Math.min((d - dz) / (r * K_FULL - dz), 1) / d;
            ax[2 * i] = Math.round(o[0] * m * 32767); ax[2 * i + 1] = Math.round(-o[1] * m * 32767);   // the host: up is positive
        }
        long pressed = (face | dpad | btn) & ~(faceBits | dpadBits | btnBits);
        int gripping = (stick[0] >= 0 ? 1 : 0) | (stick[1] >= 0 ? 2 : 0);
        if (pressed != 0 || (swap && !swapHeld) || (menu && !menuHeld) || (gripping & ~hapticSticks) != 0) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        hapticSticks = gripping;
        faceBits = face; dpadBits = dpad; btnBits = btn; swapHeld = swap; menuHeld = menu;
        nativeTouchPad(face | dpad | btn, ax[0], ax[1], ax[2], ax[3]);
        lastBtn = face | dpad | btn; lastAx = ax;
        invalidate();
    }
    void pushGame() {   // the game's touch screen: normalized on the centred 16:9 picture
        float w = getWidth(), h = getHeight(); if (w <= 0 || h <= 0) return;
        if (w / h > 16f / 9) w = h * 16 / 9; else h = w * 9 / 16;
        float x0 = (getWidth() - w) / 2, y0 = (getHeight() - h) / 2;
        int[] ids = new int[16]; float[] xs = new float[16], ys = new float[16]; int n = 0;
        for (int i = 0; i < 16; i++) if (game[i] >= 0) {
            ids[n] = i; xs[n] = Math.min(Math.max((px[game[i]] - x0) / w, 0), 0.999f); ys[n] = Math.min(Math.max((py[game[i]] - y0) / h, 0), 0.999f); n++;
        }
        nativeGameTouch(ids, xs, ys, n); if (n > 0) gameTouches++;
    }
    boolean gameBegin(int p) { for (int i = 0; i < 16; i++) if (game[i] < 0) { game[i] = p; owner[p] = C_GAME | (long)i << 8; return true; } return false; }
    void begin(int p, float x, float y) {
        down[p] = true; px[p] = x; py[p] = y; owner[p] = C_NONE; faceOf[p] = 0;
        boolean g = false;
        if (inMenu(x, y)) owner[p] = C_MENU;   // shown with a controller too (the pad hidden): the way back to the launcher
        else if (padOff) g = gameBegin(p);
        else {
            int hit = tap ? -1 : btnAt(x, y);
            if (hit >= 0) owner[p] = C_BTN | (long)hit << 8;
            else if (inSwap(x, y)) owner[p] = C_SWAP;
            else if (tap) g = gameBegin(p);
            else {   // the diamond, the D-pad, the left stick: the smallest distance / grab radius, if it is <= 1
                float best = 1; int kind = C_NONE, idx = 0;
                float f = dist(x, y, faceCx(), faceCy()) / (faceGap() + faceR() * 1.6f); if (f <= best) { best = f; kind = C_FACE; }
                if (full) { float d = dist(x, y, dpadCx(), dpadCy()) / (dpadR() * K_DPAD_GRAB); if (d < best) { best = d; kind = C_DPAD; } }
                if (stick[0] < 0) { float s = dist(x, y, homeX(), homeY()) / (stickR(0) * K_STICK_GRAB); if (s < best) { best = s; kind = C_STICK; } }
                if (kind == C_NONE && full && stick[1] < 0 && inRightArea(x)) { kind = C_STICK; idx = 1; rcx = x; rcy = y; }
                if (kind == C_FACE) { owner[p] = C_FACE; faceOf[p] = faceAt(x, y); }
                else if (kind == C_DPAD) owner[p] = C_DPAD;
                else if (kind == C_STICK) { owner[p] = C_STICK | (long)idx << 8; stick[idx] = p; stickX[idx] = x; stickY[idx] = y; }
                else g = gameBegin(p);
            }
        }
        if (g) pushGame();
    }
    void move(int p, float x, float y) {
        if (!down[p]) return;
        px[p] = x; py[p] = y; int c = (int)(owner[p] & 0xff);
        if (c == C_FACE) faceOf[p] = faceAt(x, y);
        else if (c == C_STICK) { int i = (int)(owner[p] >> 8); stickX[i] = x; stickY[i] = y;
            if (i == 1) { float d = dist(x, y, rcx, rcy), r = stickR(1); if (d > r) { rcx = x - (x - rcx) * r / d; rcy = y - (y - rcy) * r / d; } } }
    }
    boolean end(int p, boolean ended) {   // → the layout button was released on it
        if (!down[p]) return false;
        boolean swap = ended && (owner[p] & 0xff) == C_SWAP && inSwap(px[p], py[p]);
        if (ended && (owner[p] & 0xff) == C_MENU && inMenu(px[p], py[p])) menuGo = true;   // released on it
        if ((owner[p] & 0xff) == C_GAME) game[(int)(owner[p] >> 8) & 15] = -1;
        for (int i = 0; i < 2; i++) if (stick[i] == p) stick[i] = -1;
        down[p] = false; owner[p] = 0; faceOf[p] = 0;
        return swap;
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        int a = e.getActionMasked(), ai = e.getActionIndex(); boolean swap = false, gameMoved = false;
        switch (a) {
            case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: { int p = e.getPointerId(ai) & 63; begin(p, e.getX(ai), e.getY(ai)); break; }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) { int p = e.getPointerId(i) & 63; move(p, e.getX(i), e.getY(i)); if ((owner[p] & 0xff) == C_GAME) gameMoved = true; }
                break;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: { int p = e.getPointerId(ai) & 63; boolean g = (owner[p] & 0xff) == C_GAME; swap = end(p, true); if (g) pushGame(); break; }
            case MotionEvent.ACTION_CANCEL: for (int p = 0; p < 64; p++) end(p, false); pushGame(); break;
        }
        if (gameMoved) pushGame();
        recompute();
        if (swap) nextLayout();
        if (menuGo) { menuGo = false; toLauncher(); }
        return true;
    }
    void nextLayout() {   // 2D → 3D (→ Tap in handheld mode) → 2D; every touch released first
        int l = tap ? 0 : full ? (nativeOpMode() == 0 ? 2 : 0) : 1;
        reset();
        full = l == 1; tap = l == 2;
        prefs.edit().putInt("DmPadLayout", l).apply();
        invalidate();
    }
    void reset() { for (int p = 0; p < 64; p++) end(p, false); java.util.Arrays.fill(game, -1); pushGame(); recompute(); }

    // ---- Drawing ----
    void shape(Canvas c, Path p, boolean on) { fill.setColor(on ? ON : IDLE); c.drawPath(p, fill); c.drawPath(p, line); }
    void label(Canvas c, String s, float x, float y, float size, boolean on) {
        if (s == null) return;
        text.setTextSize(size); text.setColor(on ? TEXT_ON : TEXT);
        Paint.FontMetrics fm = text.getFontMetrics(); c.drawText(s, x, y - (fm.ascent + fm.descent) / 2, text);
    }
    static Path circle(float x, float y, float r) { Path p = new Path(); p.addCircle(x, y, r, Path.Direction.CW); return p; }
    static Path drop(float cx, float cy, float r, double tip) {   // a teardrop: circle and two tangents meeting at the tip
        double d = r * K_DROP_TIP, th = Math.acos(r / d);
        Path p = new Path();
        p.moveTo((float)(cx + r * Math.cos(tip - th)), (float)(cy + r * Math.sin(tip - th)));
        p.lineTo((float)(cx + d * Math.cos(tip)), (float)(cy + d * Math.sin(tip)));
        p.lineTo((float)(cx + r * Math.cos(tip + th)), (float)(cy + r * Math.sin(tip + th)));
        p.arcTo(new RectF(cx - r, cy - r, cx + r, cy + r), (float)Math.toDegrees(tip + th), (float)(360 - Math.toDegrees(2 * th)));
        p.close(); return p;
    }
    static Path arm(float cx, float cy, float r, float w, float in, double angle) {   // a D-pad arm: an upward pentagon (tip toward the centre), rotated
        float[][] v = {{-w, -r}, {w, -r}, {w, -(in + w)}, {0, -in}, {-w, -(in + w)}};
        double cs = Math.cos(angle), sn = Math.sin(angle); Path p = new Path();
        for (int i = 0; i < 5; i++) { float x = (float)(cx + v[i][0] * cs - v[i][1] * sn), y = (float)(cy + v[i][0] * sn + v[i][1] * cs); if (i == 0) p.moveTo(x, y); else p.lineTo(x, y); }
        p.close(); return p;
    }
    // ---- Frame-rate counter (DM_SHOW_FPS=1, the settings screen's Frame-rate counter): the game's presented frames over each half second, at the top left ----
    String fps; long fpsFrames, fpsAt; Paint fpsPaint;
    void fpsTick(final Handler h) {
        long n = nativeFrames(), t = android.os.SystemClock.elapsedRealtime();
        if (fpsAt != 0 && t > fpsAt) { fps = String.format(java.util.Locale.ROOT, "%.0f fps", (n - fpsFrames) * 1000.0 / (t - fpsAt)); invalidate(); }
        fpsFrames = n; fpsAt = t;
        h.postDelayed(new Runnable() { public void run() { fpsTick(h); } }, 500);
    }
    void fpsDraw(Canvas c) {
        if (fps == null) return;
        if (fpsPaint == null) { fpsPaint = new Paint(Paint.ANTI_ALIAS_FLAG); fpsPaint.setTypeface(Typeface.DEFAULT_BOLD); fpsPaint.setShadowLayer(3, 1, 1, Color.BLACK); }
        float u = u(); fpsPaint.setTextSize(16 * u); fpsPaint.setColor(Color.argb(230, 255, 255, 255)); c.drawText(fps, 12 * u, 26 * u, fpsPaint);
    }

    @Override protected void onDraw(Canvas c) {
        fpsDraw(c);
        float u = u(); long all = faceBits | dpadBits | btnBits;
        if (!padOff) {
            if (!tap) {
                for (int b = B_A; b <= B_Y; b++) { shape(c, circle(faceX(b), faceY(b), faceR()), (all >> b & 1) != 0); label(c, NAMES[b], faceX(b), faceY(b), 20 * u, (all >> b & 1) != 0); }
                for (int b : K_BTNS) {
                    if (fullOnly(b) && !full) continue;
                    boolean on = (all >> b & 1) != 0;
                    if (isDrop(b)) { boolean r = b == B_ZR || b == B_R; shape(c, drop(dropCx(b), dropCy(b), dropR(b), r ? -Math.PI / 4 : -3 * Math.PI / 4), on); label(c, NAMES[b], dropCx(b), dropCy(b), 16 * u, on); }
                    else if (isClick(b)) { shape(c, circle(clickCx(b), clickCy(), K_CLICK_R * u), on); label(c, NAMES[b], clickCx(b), clickCy(), 13 * u, on); }
                    else { RectF r = pill(b); Path p = new Path(); p.addRoundRect(r, r.height() / 2, r.height() / 2, Path.Direction.CW); shape(c, p, on); label(c, NAMES[b], r.centerX(), r.centerY(), 18 * u, on); }
                }
                if (full) { int[] arms = {B_UP, B_RIGHT, B_DOWN, B_LEFT}; for (int i = 0; i < 4; i++) shape(c, arm(dpadCx(), dpadCy(), dpadR(), K_ARM_W * u, K_ARM_IN * u, i * Math.PI / 2), (all >> arms[i] & 1) != 0); }
                fill.setColor(RING); c.drawCircle(homeX(), homeY(), stickR(0), fill); c.drawCircle(homeX(), homeY(), stickR(0), line);
                float[] o = knob(0); float k = K_KNOB_R * u;
                shape(c, circle(homeX() + o[0], homeY() + o[1], k), stick[0] >= 0); c.drawCircle(homeX() + o[0], homeY() + o[1], k * 0.62f, line);
            }
            shape(c, circle(swapCx(), swapCy(), K_SWAP_R * u), swapHeld); label(c, tap ? "Tap" : full ? "3D" : "2D", swapCx(), swapCy(), 12 * u, swapHeld);
        }
        shape(c, circle(menuCx(), menuCy(), K_SWAP_R * u), menuHeld); label(c, "≡", menuCx(), menuCy(), 18 * u, menuHeld);
    }

    // The launcher in front (the game's process keeps running behind it): Play there resumes this game, or asks before closing it for another (Launcher.java)
    void toLauncher() {
        Context a = getContext(); android.content.Intent i = new android.content.Intent(a, Launcher.class);
        i.addFlags(android.content.Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
        a.startActivity(i); nativeLog("gamepad: menu: back to the launcher");
    }

    // ---- Self-check (env.txt DM_PAD_TEST=1): touches made up and sent to this view (not to the screen), each control's result checked ----
    int checks, fails; long downT;
    MotionEvent ev(int action, int[] ids, float[][] at) {
        MotionEvent.PointerProperties[] pp = new MotionEvent.PointerProperties[ids.length]; MotionEvent.PointerCoords[] pc = new MotionEvent.PointerCoords[ids.length];
        for (int i = 0; i < ids.length; i++) { pp[i] = new MotionEvent.PointerProperties(); pp[i].id = ids[i]; pp[i].toolType = MotionEvent.TOOL_TYPE_FINGER; pc[i] = new MotionEvent.PointerCoords(); pc[i].x = at[i][0]; pc[i].y = at[i][1]; pc[i].pressure = 1; pc[i].size = 1; }
        return MotionEvent.obtain(downT, android.os.SystemClock.uptimeMillis(), action, ids.length, pp, pc, 0, 0, 1, 1, 0, 0, android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
    }
    void send(MotionEvent e) { onTouchEvent(e); e.recycle(); }
    void press(float x, float y) { downT = android.os.SystemClock.uptimeMillis(); send(ev(MotionEvent.ACTION_DOWN, new int[]{0}, new float[][]{{x, y}})); }
    void release(float x, float y) { send(ev(MotionEvent.ACTION_UP, new int[]{0}, new float[][]{{x, y}})); }
    void check(String what, boolean ok) { checks++; if (!ok) fails++; nativeLog("gamepad check: " + what + " " + (ok ? "ok" : "FAILED")); }
    void tapCheck(String what, float x, float y, long want) { press(x, y); check(what + " pressed", lastBtn == want); release(x, y); check(what + " released", lastBtn == 0); }
    void selfCheck() {
        checks = fails = 0; reset();
        if (padOff) { nativeLog("gamepad check: a controller is connected, the touch controls are hidden: not checked"); return; }
        while (!full || tap) { press(swapCx(), swapCy()); release(swapCx(), swapCy()); }   // to the 3D layout
        check("the layout button gives 3D", full && !tap);
        String[] fn = {"A", "B", "X", "Y"};
        for (int b = B_A; b <= B_Y; b++) tapCheck(fn[b], faceX(b), faceY(b), 1L << b);
        for (int b : K_BTNS) {
            float x = isDrop(b) ? dropCx(b) : isClick(b) ? clickCx(b) : pill(b).centerX(), y = isDrop(b) ? dropCy(b) : isClick(b) ? clickCy() : pill(b).centerY();
            tapCheck(NAMES[b], x, y, 1L << b);
        }
        float r = dpadR() * 0.7f;
        tapCheck("D-pad up", dpadCx(), dpadCy() - r, 1L << B_UP); tapCheck("D-pad right", dpadCx() + r, dpadCy(), 1L << B_RIGHT);
        tapCheck("D-pad down", dpadCx(), dpadCy() + r, 1L << B_DOWN); tapCheck("D-pad left", dpadCx() - r, dpadCy(), 1L << B_LEFT);
        tapCheck("D-pad up-right", dpadCx() + r * 0.7f, dpadCy() - r * 0.7f, 1L << B_UP | 1L << B_RIGHT);
        // the left stick: pushed right to the ring edge, then up; released to the centre
        press(homeX(), homeY()); send(ev(MotionEvent.ACTION_MOVE, new int[]{0}, new float[][]{{homeX() + stickR(0), homeY()}}));
        check("left stick right = +32767", lastAx[0] == 32767 && lastAx[1] == 0);
        send(ev(MotionEvent.ACTION_MOVE, new int[]{0}, new float[][]{{homeX(), homeY() - stickR(0)}}));
        check("left stick up = +32767 (up is positive)", lastAx[0] == 0 && lastAx[1] == 32767);
        release(homeX(), homeY() - stickR(0)); check("left stick released = 0", lastAx[0] == 0 && lastAx[1] == 0);
        // the right stick: a touch in the right third on no control, centred where it lands
        float rx = getWidth() - getWidth() * K_RSWIPE / 2, ry = getHeight() * 0.45f;
        press(rx, ry); send(ev(MotionEvent.ACTION_MOVE, new int[]{0}, new float[][]{{rx - stickR(1), ry}}));
        check("right stick left = -32767", lastAx[2] == -32767 && lastAx[3] == 0);
        release(rx - stickR(1), ry); check("right stick released = 0", lastAx[2] == 0 && lastAx[3] == 0);
        // two fingers: A held while the left stick moves
        downT = android.os.SystemClock.uptimeMillis();
        send(ev(MotionEvent.ACTION_DOWN, new int[]{0}, new float[][]{{faceX(B_A), faceY(B_A)}}));
        send(ev(MotionEvent.ACTION_POINTER_DOWN | 1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT, new int[]{0, 1}, new float[][]{{faceX(B_A), faceY(B_A)}, {homeX(), homeY()}}));
        send(ev(MotionEvent.ACTION_MOVE, new int[]{0, 1}, new float[][]{{faceX(B_A), faceY(B_A)}, {homeX() + stickR(0), homeY()}}));
        check("two fingers: A held and the left stick right", lastBtn == 1L << B_A && lastAx[0] == 32767);
        send(ev(MotionEvent.ACTION_POINTER_UP, new int[]{0, 1}, new float[][]{{faceX(B_A), faceY(B_A)}, {homeX() + stickR(0), homeY()}}));
        check("first finger up: the stick stays, A released", lastBtn == 0 && lastAx[0] == 32767);
        send(ev(MotionEvent.ACTION_UP, new int[]{1}, new float[][]{{homeX() + stickR(0), homeY()}}));
        // the game's touch screen: a touch in the middle, on no control
        int g = gameTouches; press(getWidth() / 2f, getHeight() / 2f); check("a touch in the middle goes to the game's touch screen", gameTouches > g && game[0] >= 0);
        release(getWidth() / 2f, getHeight() / 2f); check("and is lifted", game[0] < 0);
        nativeLog("gamepad check: " + checks + " checks, " + fails + " failed");
        while (full || tap) { press(swapCx(), swapCy()); release(swapCx(), swapCy()); }   // back to 2D
    }
    int gameTouches;

    // ---- Physical controllers: the controls hide while one is connected ----
    static boolean anyPad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            // a game controller gets a controller number (the phone's own keys and sensors that call themselves joysticks do not)
            if (d != null && !d.isVirtual() && d.getControllerNumber() > 0 && ((d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (d.getSources() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)) return true;
        }
        return false;
    }
    void padsChanged() { boolean any = anyPad(); if (any != padOff) { padOff = any; reset(); invalidate(); nativeLog("gamepad: " + (any ? "a controller is connected: the touch controls hide" : "no controller: touch controls")); } }

    // ---- Start (front_android.c, from the native thread) ----
    static Gamepad view; static PopupWindow popup;
    public static void start(final Activity a) {
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() { public void run() {
            final View decor = a.getWindow().getDecorView();
            if (decor.getWindowToken() == null) { h.postDelayed(this, 100); return; }   // the activity's window is not attached yet
            ctx = a.getApplicationContext(); view = new Gamepad(a);
            popup = new PopupWindow(view, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, false);
            popup.setTouchable(true); popup.setFocusable(false); popup.setClippingEnabled(false);
            if (android.os.Build.VERSION.SDK_INT >= 22) popup.setAttachedInDecor(false);
            popup.showAtLocation(decor, Gravity.NO_GRAVITY, 0, 0);
            InputManager im = (InputManager)a.getSystemService(Context.INPUT_SERVICE);
            im.registerInputDeviceListener(new InputManager.InputDeviceListener() {
                public void onInputDeviceAdded(int id) { view.padsChanged(); }
                public void onInputDeviceRemoved(int id) { view.padsChanged(); }
                public void onInputDeviceChanged(int id) { view.padsChanged(); }
            }, h);
            view.padsChanged();
            shakeStart(a);
            nativeLog("gamepad: touch controls shown (" + (view.tap ? "Tap" : view.full ? "3D" : "2D") + ")");
            if ("1".equals(nativeEnv("DM_SHOW_FPS"))) view.fpsTick(h);
            String t = nativeEnv("DM_PAD_TEST"); if (t != null && !t.equals("0")) h.postDelayed(new Runnable() { public void run() { view.selfCheck(); } }, 1500);
        } });
    }
    public static void reset_() { new Handler(Looper.getMainLooper()).post(new Runnable() { public void run() { if (view != null) view.reset(); } }); }   // the app went to the background: nothing stays held

    // ---- Game vibration (haptics_android.c): the connected controller's vibrator, else the phone's; a one-shot of a second at the level, renewed by the caller while it lasts ----
    static Context ctx;
    public static void rumble(float level) {
        android.os.Vibrator v = null;
        for (int id : InputDevice.getDeviceIds()) { InputDevice d = InputDevice.getDevice(id);
            if (d != null && d.getControllerNumber() > 0 && d.getVibrator() != null && d.getVibrator().hasVibrator()) { v = d.getVibrator(); break; } }
        if (v == null && ctx != null) v = (android.os.Vibrator)ctx.getSystemService(Context.VIBRATOR_SERVICE);
        if (v == null || !v.hasVibrator()) return;
        if (level <= 0) { v.cancel(); return; }
        int amp = v.hasAmplitudeControl() ? Math.max(1, Math.min(255, Math.round(level * 255))) : android.os.VibrationEffect.DEFAULT_AMPLITUDE;
        // a game's vibration, not touch feedback: the phone's setting for touch vibration does not silence it
        v.vibrate(android.os.VibrationEffect.createOneShot(1000, amp), new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_GAME).build());
    }

    // ---- Shaking the phone ----
    static void shakeStart(Context c) {
        SensorManager sm = (SensorManager)c.getSystemService(Context.SENSOR_SERVICE);
        Sensor acc = sm == null ? null : sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (acc == null) { nativeLog("gamepad: no accelerometer, shaking the phone does nothing"); return; }
        String e = System.getenv("DM_SHAKE_G"); double t = 1.5; try { if (e != null && Double.parseDouble(e) > 0) t = Double.parseDouble(e); } catch (NumberFormatException x) { }
        final double thr = t;
        sm.registerListener(new SensorEventListener() {
            double gx, gy, gz, peak; long last = -1; boolean init, on;
            public void onSensorChanged(SensorEvent ev) {
                double x = ev.values[0] / 9.81, y = ev.values[1] / 9.81, z = ev.values[2] / 9.81;
                if (!init) { gx = x; gy = y; gz = z; init = true; }
                double g = Math.sqrt((x - gx) * (x - gx) + (y - gy) * (y - gy) + (z - gz) * (z - gz));
                gx += (x - gx) * 0.02; gy += (y - gy) * 0.02; gz += (z - gz) * 0.02;
                if (g > thr) { if (last < 0) peak = 0; last = ev.timestamp; peak = Math.max(peak, g); }
                boolean now = last >= 0 && ev.timestamp - last < 120000000L;
                if (!now && last >= 0) { nativeLog(String.format("gamepad: phone shaken (peak %.1f G)", peak)); last = -1; }
                if (now != on) { on = now; nativeShake(now); }
            }
            public void onAccuracyChanged(Sensor s, int a) { }
        }, acc, 16667, new Handler(Looper.getMainLooper()));
        nativeLog(String.format("gamepad: shaking the phone shakes the controller (threshold %.2f G)", thr));
    }

    // ---- The software keyboard's text box (front_text_input): shown on the UI thread, the caller blocks until it is confirmed (the text) or cancelled (null) ----
    public static String ask(final Activity a, final String title, final String sub, final String guide, final String initial, final int maxChars) {
        final String[] out = {null}; final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() { public void run() {
            LinearLayout box = new LinearLayout(a); box.setOrientation(LinearLayout.VERTICAL); int pad = (int)(20 * a.getResources().getDisplayMetrics().density); box.setPadding(pad, pad / 2, pad, 0);
            if (sub != null && !sub.isEmpty()) { TextView t = new TextView(a); t.setText(sub); box.addView(t); }
            final EditText in = new EditText(a); in.setSingleLine(true); in.setText(initial == null ? "" : initial); in.setSelection(in.getText().length());
            if (guide != null) in.setHint(guide);
            if (maxChars > 0) in.setFilters(new InputFilter[]{new InputFilter.LengthFilter(maxChars)});
            box.addView(in);
            AlertDialog d = new AlertDialog.Builder(a).setTitle(title == null || title.isEmpty() ? null : title).setView(box)
                .setPositiveButton(android.R.string.ok, new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface di, int w) { out[0] = in.getText().toString(); done.countDown(); } })
                .setNegativeButton(android.R.string.cancel, new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface di, int w) { done.countDown(); } })
                .setOnCancelListener(new android.content.DialogInterface.OnCancelListener() { public void onCancel(android.content.DialogInterface di) { done.countDown(); } }).create();
            d.getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            d.show(); in.requestFocus();
            if ("2".equals(nativeEnv("DM_TEXT_TEST"))) {   // the check of the box (front_android.c): typed and confirmed as a person would, after a second
                final AlertDialog dd = d;
                new Handler(Looper.getMainLooper()).postDelayed(new Runnable() { public void run() { in.setText("Zelda"); dd.getButton(AlertDialog.BUTTON_POSITIVE).performClick(); } }, 1000);
            }
        } });
        try { done.await(); } catch (InterruptedException e) { return null; }
        return out[0];
    }
}
