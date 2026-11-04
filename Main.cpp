// LVUP v4.4.1 - native mod menu v2 (pengganti libbmt.so)
#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <link.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <string>
#include <atomic>
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "LVMOD", __VA_ARGS__)

extern "C" int DobbyHook(void *address, void *replace_func, void **origin_func);

// ===== alamat fungsi di libil2cpp.so (LVUP 4.4.1) =====
static const uintptr_t RVA_GM_FixedUpdate   = 0x24a4644;
static const uintptr_t RVA_GM_UpdateGold    = 0x24a9900;
static const uintptr_t RVA_GM_UpdateCrystal = 0x24a5af8;
static const uintptr_t RVA_GM_UpdateExp     = 0x24a9c44;
static const uintptr_t RVA_GM_UpdateItem    = 0x24aa3c8;
static const uintptr_t RVA_GM_UpdateWing    = 0x24a0a38;
static const uintptr_t RVA_GM_UpdateAcc     = 0x24aac34;
static const uintptr_t RVA_GM_FullHeal      = 0x24a5cd4;
static const uintptr_t RVA_PC_FixedUpdate   = 0x250dd88;
static const uintptr_t RVA_PC_Damege        = 0x2514464;
static const uintptr_t RVA_PC_AbsDamege     = 0x250f4dc;
static const uintptr_t RVA_PC_UserDamege    = 0x2514cd4;
static const uintptr_t RVA_DDD_Damege       = 0x23aa068;
static const uintptr_t RVA_MMC_Damege       = 0x25077a8;
static const uintptr_t RVA_ASMC_Damege      = 0x24f8a3c;
static const uintptr_t RVA_ABMC_Damege      = 0x24f24d0;
static const uintptr_t RVA_AMC_Damege       = 0x24f63e8;
static const uintptr_t RVA_DDDASMC_Damege   = 0x2503878;
// ObscuredFloat (ACTk.Runtime.dll)
static const uintptr_t RVA_OBS_InternalDecrypt = 0x2347ce4;
static const uintptr_t RVA_OBS_GetDecrypted    = 0x2347e3c;
static const uintptr_t RVA_OBS_OpImplicit      = 0x233c59c;

// offset field di MainGameManager / MainPlayerController
static const size_t OFF_GM_Wings = 0x380, OFF_GM_Accs = 0x3a0;
static const size_t OFF_GM_DebugWing = 0xd70, OFF_GM_DebugAcc = 0xd71;
static const size_t OFF_PC_MoveSpeed = 0x28;

// ===== status fitur =====
static std::atomic<bool> g_loaded(false);
static volatile bool godMode = false, oneHit = false, autoHeal = false, dbgUnlock = false;
static volatile int dmgMul = 1, defMul = 1, playerSpeed = 1;
static volatile int lootMul = 1, goldMul = 1, expMul = 1, crystalMul = 1;
static std::atomic<int> pGold(0), pCrystal(0), pExp(0);
static std::atomic<bool> pHeal(false), pWings(false), pAccs(false);
static time_t g_start = 0;
static void *g_pc = nullptr;
static volatile bool g_selfCall = false;

// ===== fungsi game yang dipanggil =====
static void (*GM_UpdateGold)(void *, int, const void *) = nullptr;
static void (*GM_UpdateCrystal)(void *, int, const void *) = nullptr;
static void (*GM_UpdateExp)(void *, int, const void *) = nullptr;
static void (*GM_UpdateWing)(void *, int, const void *) = nullptr;
static void (*GM_UpdateAcc)(void *, int, const void *) = nullptr;
static void (*GM_FullHeal)(void *, const void *) = nullptr;

static void giveAll(void *gm, size_t off, void (*fn)(void *, int, const void *)) {
    if (!fn) return;
    void *arr = *(void **)((char *)gm + off);
    if (!arr) return;
    uint64_t len = *(uint64_t *)((char *)arr + 0x18);
    if (len == 0 || len > 300) return;
    for (uint64_t i = 0; i < len; i++) fn(gm, (int)i, nullptr);
}

// dijalankan di thread utama game (aman untuk UI Unity)
static int healTick = 0;
static bool dbgWasOn = false;
static void (*o_GM_FU)(void *, const void *);
static void h_GM_FU(void *self, const void *mi) {
    g_selfCall = true;
    int g = pGold.exchange(0);
    if (g != 0 && GM_UpdateGold) GM_UpdateGold(self, g, nullptr);
    int c = pCrystal.exchange(0);
    if (c != 0 && GM_UpdateCrystal) GM_UpdateCrystal(self, c, nullptr);
    int e = pExp.exchange(0);
    if (e != 0 && GM_UpdateExp) GM_UpdateExp(self, e, nullptr);
    if (pWings.exchange(false)) giveAll(self, OFF_GM_Wings, GM_UpdateWing);
    if (pAccs.exchange(false)) giveAll(self, OFF_GM_Accs, GM_UpdateAcc);
    g_selfCall = false;

    if (autoHeal) { if (++healTick >= 100) { healTick = 0; pHeal = true; } } else healTick = 0;
    if (pHeal.exchange(false) && GM_FullHeal) GM_FullHeal(self, nullptr);

    if (dbgUnlock) {
        *(bool *)((char *)self + OFF_GM_DebugWing) = true;
        *(bool *)((char *)self + OFF_GM_DebugAcc) = true;
        dbgWasOn = true;
    } else if (dbgWasOn) {
        *(bool *)((char *)self + OFF_GM_DebugWing) = false;
        *(bool *)((char *)self + OFF_GM_DebugAcc) = false;
        dbgWasOn = false;
    }
    o_GM_FU(self, mi);
}

// tangkap instance pemain
static void (*o_PC_FU)(void *, const void *);
static void h_PC_FU(void *self, const void *mi) {
    g_pc = self;
    o_PC_FU(self, mi);
}

// ===== kecepatan gerak pemain (baca ObscuredFloat m_moveSpeed) =====
static thread_local int g_decDepth = 0;
static inline float decHook(float (*o)(void *, const void *), void *self, const void *mi) {
    if (g_decDepth > 0) return o(self, mi);
    g_decDepth++;
    float v = o(self, mi);
    g_decDepth--;
    int sm = playerSpeed;
    void *pc = g_pc;
    if (sm > 1 && pc && self && *(uint64_t *)self == *(uint64_t *)((char *)pc + OFF_PC_MoveSpeed)) v *= (float)sm;
    return v;
}
static float (*o_dec1)(void *, const void *);
static float (*o_dec2)(void *, const void *);
static float (*o_dec3)(void *, const void *);
static float h_dec1(void *s, const void *m) { return decHook(o_dec1, s, m); }
static float h_dec2(void *s, const void *m) { return decHook(o_dec2, s, m); }
static float h_dec3(void *s, const void *m) { return decHook(o_dec3, s, m); }

// ===== pengali Gold / EXP / Crystal (hanya pemasukan dari game) =====
#define MULT_HOOK(NAME, MULVAR) \
    static void (*o_##NAME)(void *, int, const void *); \
    static void h_##NAME(void *s, int n, const void *m) { \
        int mul = MULVAR; \
        if (!g_selfCall && n > 0 && mul > 1) { \
            long long v = (long long)n * mul; \
            if (v > 2000000000LL) v = 2000000000LL; \
            n = (int)v; \
        } \
        o_##NAME(s, n, m); \
    }
MULT_HOOK(Gold, goldMul)
MULT_HOOK(Crystal, crystalMul)
MULT_HOOK(Exp, expMul)

// ===== pengali loot: bahan yang dijatuhkan monster =====
static bool (*o_UpdateItem)(void *, int, int, bool, const void *);
static bool h_UpdateItem(void *s, int item, int add, bool isDrop, const void *m) {
    int mul = lootMul;
    if (isDrop && add > 0 && item >= 0 && item < 175 && mul > 1) {
        long long v = (long long)add * mul;
        if (v > 1000000000LL) v = 1000000000LL;
        add = (int)v;
    }
    return o_UpdateItem(s, item, add, isDrop, m);
}

// ===== hook generik: semua register argumen diteruskan apa adanya =====
typedef void (*G8)(void *, long, long, long, long, long, long, long, float, float, float, float, float, float, float, float);
#define GARGS void *s, long a1, long a2, long a3, long a4, long a5, long a6, long a7, float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7
#define GPASS s, a1, a2, a3, a4, a5, a6, a7, f0, f1, f2, f3, f4, f5, f6, f7

// kerusakan yang diterima pemain: god mode + defense multiplier + auto heal saat kena hit
#define PLAYER_HOOK(NAME) \
    static G8 o_##NAME; \
    static void h_##NAME(GARGS) { \
        if (godMode) return; \
        int dm = defMul; \
        if (dm > 1) { a1 = (long)(unsigned)((int)a1 / dm); f0 /= (float)dm; } \
        o_##NAME(GPASS); \
        if (autoHeal) pHeal = true; \
    }

// kerusakan ke monster: damage multiplier + one hit kill
static inline void applyDmg(long &a1, long &a2) {
    long long d = (int)a1, sk = (int)a2;
    int mul = dmgMul;
    if (oneHit) { d = 300000000; }
    else if (mul > 1) { d *= mul; sk *= mul; }
    if (d > 1000000000) d = 1000000000;
    if (sk > 1000000000) sk = 1000000000;
    a1 = (long)(unsigned)(int)d;
    a2 = (long)(unsigned)(int)sk;
}
#define MON_HOOK(NAME) \
    static G8 o_##NAME; \
    static void h_##NAME(GARGS) { applyDmg(a1, a2); o_##NAME(GPASS); }

PLAYER_HOOK(PC_Damege)
PLAYER_HOOK(PC_AbsDamege)
PLAYER_HOOK(PC_UserDamege)
PLAYER_HOOK(DDD_Damege)
MON_HOOK(MMC)
MON_HOOK(ASMC)
MON_HOOK(ABMC)
MON_HOOK(AMC)
MON_HOOK(DDDASMC)

// ===== pasang hook =====
static int phdr_cb(struct dl_phdr_info *info, size_t, void *data) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
        *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
        return 1;
    }
    return 0;
}
static uintptr_t findIl2cpp() {
    uintptr_t b = 0;
    dl_iterate_phdr(phdr_cb, &b);
    return b;
}
static void hk(uintptr_t base, uintptr_t rva, void *fn, void **orig, const char *name) {
    int r = DobbyHook((void *)(base + rva), fn, orig);
    LOGI("hook %s = %d", name, r);
}

static void *hack_thread(void *) {
    uintptr_t base = 0;
    for (int i = 0; i < 900 && !base; i++) {
        base = findIl2cpp();
        if (!base) sleep(1);
    }
    if (!base) return nullptr;
    LOGI("libil2cpp base = %p", (void *)base);
    sleep(5);
    GM_UpdateGold = (decltype(GM_UpdateGold))(base + RVA_GM_UpdateGold);
    GM_UpdateCrystal = (decltype(GM_UpdateCrystal))(base + RVA_GM_UpdateCrystal);
    GM_UpdateExp = (decltype(GM_UpdateExp))(base + RVA_GM_UpdateExp);
    GM_UpdateWing = (decltype(GM_UpdateWing))(base + RVA_GM_UpdateWing);
    GM_UpdateAcc = (decltype(GM_UpdateAcc))(base + RVA_GM_UpdateAcc);
    GM_FullHeal = (decltype(GM_FullHeal))(base + RVA_GM_FullHeal);

    hk(base, RVA_GM_FixedUpdate, (void *)h_GM_FU, (void **)&o_GM_FU, "GM_FixedUpdate");
    hk(base, RVA_PC_FixedUpdate, (void *)h_PC_FU, (void **)&o_PC_FU, "PC_FixedUpdate");
    hk(base, RVA_GM_UpdateGold, (void *)h_Gold, (void **)&o_Gold, "UpdateGold");
    hk(base, RVA_GM_UpdateCrystal, (void *)h_Crystal, (void **)&o_Crystal, "UpdateCrystal");
    hk(base, RVA_GM_UpdateExp, (void *)h_Exp, (void **)&o_Exp, "UpdateExp");
    hk(base, RVA_GM_UpdateItem, (void *)h_UpdateItem, (void **)&o_UpdateItem, "UpdateItem");
    hk(base, RVA_PC_Damege, (void *)h_PC_Damege, (void **)&o_PC_Damege, "PC_Damege");
    hk(base, RVA_PC_AbsDamege, (void *)h_PC_AbsDamege, (void **)&o_PC_AbsDamege, "PC_AbsDamege");
    hk(base, RVA_PC_UserDamege, (void *)h_PC_UserDamege, (void **)&o_PC_UserDamege, "PC_UserDamege");
    hk(base, RVA_DDD_Damege, (void *)h_DDD_Damege, (void **)&o_DDD_Damege, "DDD_Damege");
    hk(base, RVA_MMC_Damege, (void *)h_MMC, (void **)&o_MMC, "MMC");
    hk(base, RVA_ASMC_Damege, (void *)h_ASMC, (void **)&o_ASMC, "ASMC");
    hk(base, RVA_ABMC_Damege, (void *)h_ABMC, (void **)&o_ABMC, "ABMC");
    hk(base, RVA_AMC_Damege, (void *)h_AMC, (void **)&o_AMC, "AMC");
    hk(base, RVA_DDDASMC_Damege, (void *)h_DDDASMC, (void **)&o_DDDASMC, "DDDASMC");
    hk(base, RVA_OBS_InternalDecrypt, (void *)h_dec1, (void **)&o_dec1, "OBS_InternalDecrypt");
    hk(base, RVA_OBS_GetDecrypted, (void *)h_dec2, (void **)&o_dec2, "OBS_GetDecrypted");
    hk(base, RVA_OBS_OpImplicit, (void *)h_dec3, (void **)&o_dec3, "OBS_OpImplicit");
    g_loaded = true;
    LOGI("semua hook terpasang");
    return nullptr;
}

// ===== JNI (kontrak dengan menu Java bawaan) =====
static const char *kIcon = "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";

static jobjectArray makeArray(JNIEnv *env, const char **arr, int n) {
    jclass sc = env->FindClass("java/lang/String");
    jobjectArray ret = env->NewObjectArray(n, sc, env->NewStringUTF(""));
    for (int i = 0; i < n; i++) env->SetObjectArrayElement(ret, i, env->NewStringUTF(arr[i]));
    return ret;
}

static jobjectArray JNICALL GetFeatureList(JNIEnv *env, jobject) {
    static const char *f[] = {
        "Category_Combat",
        "0_Toggle_God Mode",
        "1_SeekBar_Damage Multiplier_1_100",
        "2_SeekBar_Defense Multiplier_1_100",
        "6_Toggle_One Hit Kill",
        "8_Toggle_Auto Heal",
        "7_SeekBar_Player Speed_1_4",
        "Category_Resources",
        "3_InputValue_2000000000_Add Gold",
        "4_InputValue_2000000000_Add Crystal",
        "5_InputValue_2000000000_Add EXP",
        "12_SeekBar_Loot Multiplier_1_50",
        "13_SeekBar_Gold Multiplier_1_100",
        "14_SeekBar_EXP Multiplier_1_100",
        "15_SeekBar_Crystal Multiplier_1_100",
        "Category_Collection",
        "9_Button_Get All Wings",
        "10_Button_Get All Accessories",
        "11_Toggle_Debug Unlock Wings & Accessories"};
    return makeArray(env, f, sizeof(f) / sizeof(f[0]));
}

static jobjectArray JNICALL SettingsList(JNIEnv *env, jobject) {
    static const char *f[] = {
        "Category_Settings",
        "-1_Toggle_Save feature preferences",
        "-3_Toggle_Auto size vertically",
        "-6_Button_<font color='red'>Close settings</font>"};
    return makeArray(env, f, sizeof(f) / sizeof(f[0]));
}

static jstring JNICALL Icon(JNIEnv *env, jobject) { return env->NewStringUTF(kIcon); }
static jstring JNICALL IconWebViewData(JNIEnv *, jobject) { return nullptr; }
static jboolean JNICALL IsGameLibLoaded(JNIEnv *, jobject) { return g_loaded.load() ? JNI_TRUE : JNI_FALSE; }

static void setHtml(JNIEnv *env, jobject tv, const char *html) {
    jclass hc = env->FindClass("android/text/Html");
    jmethodID fh = env->GetStaticMethodID(hc, "fromHtml", "(Ljava/lang/String;)Landroid/text/Spanned;");
    jobject sp = env->CallStaticObjectMethod(hc, fh, env->NewStringUTF(html));
    jclass tc = env->FindClass("android/widget/TextView");
    jmethodID st = env->GetMethodID(tc, "setText", "(Ljava/lang/CharSequence;)V");
    env->CallVoidMethod(tv, st, sp);
}

static void JNICALL Init(JNIEnv *env, jobject, jobject ctx, jobject title, jobject subtitle) {
    setHtml(env, title, "<b>LVUP Mod Menu</b>");
    setHtml(env, subtitle, "<b>NS Mod Menu</b>");
}

static void JNICALL CheckOverlayPermission(JNIEnv *env, jobject, jobject ctx) {
    jclass settings = env->FindClass("android/provider/Settings");
    jmethodID can = env->GetStaticMethodID(settings, "canDrawOverlays", "(Landroid/content/Context;)Z");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (env->CallStaticBooleanMethod(settings, can, ctx)) return;
    jclass cc = env->GetObjectClass(ctx);
    jmethodID gp = env->GetMethodID(cc, "getPackageName", "()Ljava/lang/String;");
    jstring pkg = (jstring)env->CallObjectMethod(ctx, gp);
    const char *p = env->GetStringUTFChars(pkg, nullptr);
    std::string u = std::string("package:") + p;
    env->ReleaseStringUTFChars(pkg, p);
    jclass uc = env->FindClass("android/net/Uri");
    jmethodID parse = env->GetStaticMethodID(uc, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jobject uri = env->CallStaticObjectMethod(uc, parse, env->NewStringUTF(u.c_str()));
    jclass ic = env->FindClass("android/content/Intent");
    jmethodID ctor = env->GetMethodID(ic, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jobject in = env->NewObject(ic, ctor, env->NewStringUTF("android.settings.action.MANAGE_OVERLAY_PERMISSION"), uri);
    jmethodID af = env->GetMethodID(ic, "addFlags", "(I)Landroid/content/Intent;");
    env->CallObjectMethod(in, af, 0x10000000);
    jmethodID sa = env->GetMethodID(cc, "startActivity", "(Landroid/content/Intent;)V");
    env->CallVoidMethod(ctx, sa, in);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static bool tooEarly() { return time(nullptr) - g_start < 10; } // abaikan pemulihan pengaturan saat game baru dibuka

static void addTo(std::atomic<int> &t, int v) {
    if (v <= 0 || tooEarly()) return;
    long long n = (long long)t.load() + v;
    if (n > 2000000000LL) n = 2000000000LL;
    t.store((int)n);
}

static inline int atLeast1(int v) { return v < 1 ? 1 : v; }

static void JNICALL Changes(JNIEnv *env, jobject, jobject ctx, jint feat, jstring name, jint value, jboolean b, jstring str) {
    switch (feat) {
        case 0: godMode = b; break;
        case 1: dmgMul = atLeast1(value); break;
        case 2: defMul = atLeast1(value); break;
        case 3: addTo(pGold, value); break;
        case 4: addTo(pCrystal, value); break;
        case 5: addTo(pExp, value); break;
        case 6: oneHit = b; break;
        case 7: playerSpeed = atLeast1(value); break;
        case 8: autoHeal = b; break;
        case 9: if (!tooEarly()) pWings = true; break;
        case 10: if (!tooEarly()) pAccs = true; break;
        case 11: dbgUnlock = b; break;
        case 12: lootMul = atLeast1(value); break;
        case 13: goldMul = atLeast1(value); break;
        case 14: expMul = atLeast1(value); break;
        case 15: crystalMul = atLeast1(value); break;
        default: break;
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    g_start = time(nullptr);

    JNINativeMethod menu[] = {
        {"Icon", "()Ljava/lang/String;", (void *)Icon},
        {"IconWebViewData", "()Ljava/lang/String;", (void *)IconWebViewData},
        {"IsGameLibLoaded", "()Z", (void *)IsGameLibLoaded},
        {"Init", "(Landroid/content/Context;Landroid/widget/TextView;Landroid/widget/TextView;)V", (void *)Init},
        {"SettingsList", "()[Ljava/lang/String;", (void *)SettingsList},
        {"GetFeatureList", "()[Ljava/lang/String;", (void *)GetFeatureList}};
    JNINativeMethod prefs[] = {
        {"Changes", "(Landroid/content/Context;ILjava/lang/String;IZLjava/lang/String;)V", (void *)Changes}};
    JNINativeMethod mainm[] = {
        {"CheckOverlayPermission", "(Landroid/content/Context;)V", (void *)CheckOverlayPermission}};

    jclass c = env->FindClass("com/android/support/Menu");
    if (!c || env->RegisterNatives(c, menu, 6) != 0) return JNI_ERR;
    c = env->FindClass("com/android/support/Preferences");
    if (!c || env->RegisterNatives(c, prefs, 1) != 0) return JNI_ERR;
    c = env->FindClass("com/android/support/Main");
    if (!c || env->RegisterNatives(c, mainm, 1) != 0) return JNI_ERR;

    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    return JNI_VERSION_1_6;
}
