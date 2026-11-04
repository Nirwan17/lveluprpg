// LVUP v4.4.1 - native mod menu (pengganti libbmt.so)
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
static const uintptr_t RVA_resolve_icall  = 0x22380b0;
static const uintptr_t RVA_GM_FixedUpdate = 0x24a4644;
static const uintptr_t RVA_GM_UpdateGold  = 0x24a9900;
static const uintptr_t RVA_GM_UpdateCrystal = 0x24a5af8;
static const uintptr_t RVA_GM_UpdateExp   = 0x24a9c44;
static const uintptr_t RVA_GM_FullHeal    = 0x24a5cd4;
static const uintptr_t RVA_GM_AccHave     = 0x24aad20;
static const uintptr_t RVA_GM_WingHave    = 0x24aad54;
static const uintptr_t RVA_GM_MySetWing   = 0x24a7bac;
static const uintptr_t RVA_GM_MySetAcc    = 0x24a7bec;
static const uintptr_t RVA_PC_Damege      = 0x2514464;
static const uintptr_t RVA_PC_AbsDamege   = 0x250f4dc;
static const uintptr_t RVA_PC_UserDamege  = 0x2514cd4;
static const uintptr_t RVA_DDD_Damege     = 0x23aa068;
static const uintptr_t RVA_MMC_Damege     = 0x25077a8;
static const uintptr_t RVA_ASMC_Damege    = 0x24f8a3c;
static const uintptr_t RVA_ABMC_Damege    = 0x24f24d0;
static const uintptr_t RVA_AMC_Damege     = 0x24f63e8;
static const uintptr_t RVA_DDDASMC_Damege = 0x2503878;

// ===== status fitur =====
static std::atomic<bool> g_loaded(false);
static volatile bool godMode = false, oneHit = false, unlockAll = false;
static volatile int dmgMul = 1, defMul = 1, gameSpeed = 1;
static std::atomic<int> pGold(0), pCrystal(0), pExp(0);
static std::atomic<bool> pHeal(false);
static time_t g_start = 0;

// ===== icall Unity (Time.timeScale) =====
typedef void *(*resolve_icall_t)(const char *);
typedef float (*get_ts_t)();
typedef void (*set_ts_t)(float);
static get_ts_t icall_get_ts = nullptr;
static set_ts_t icall_set_ts = nullptr;
static bool speedWasOn = false;

static void resolveIcalls(uintptr_t base) {
    resolve_icall_t r = (resolve_icall_t)(base + RVA_resolve_icall);
    const char *sn[] = {"UnityEngine.Time::set_timeScale(System.Single)", "UnityEngine.Time::set_timeScale"};
    const char *gn[] = {"UnityEngine.Time::get_timeScale()", "UnityEngine.Time::get_timeScale"};
    for (const char *n : sn) { void *p = r(n); if (p) { icall_set_ts = (set_ts_t)p; break; } }
    for (const char *n : gn) { void *p = r(n); if (p) { icall_get_ts = (get_ts_t)p; break; } }
    LOGI("icall set=%p get=%p", (void *)icall_set_ts, (void *)icall_get_ts);
}

static void applySpeed() {
    if (!icall_set_ts || !icall_get_ts) return;
    int gs = gameSpeed;
    if (gs > 1) {
        float cur = icall_get_ts();
        if (cur > 0.05f && cur != (float)gs) icall_set_ts((float)gs);
        speedWasOn = true;
    } else if (speedWasOn) {
        float cur = icall_get_ts();
        if (cur > 0.05f) icall_set_ts(1.0f);
        speedWasOn = false;
    }
}

// ===== fungsi game yang dipanggil =====
static void (*GM_UpdateGold)(void *, int, const void *) = nullptr;
static void (*GM_UpdateCrystal)(void *, int, const void *) = nullptr;
static void (*GM_UpdateExp)(void *, int, const void *) = nullptr;
static void (*GM_FullHeal)(void *, const void *) = nullptr;

// dijalankan di thread utama game (aman untuk UI Unity)
static void (*o_GM_FU)(void *, const void *);
static void h_GM_FU(void *self, const void *mi) {
    int g = pGold.exchange(0);
    if (g != 0 && GM_UpdateGold) GM_UpdateGold(self, g, nullptr);
    int c = pCrystal.exchange(0);
    if (c != 0 && GM_UpdateCrystal) GM_UpdateCrystal(self, c, nullptr);
    int e = pExp.exchange(0);
    if (e != 0 && GM_UpdateExp) GM_UpdateExp(self, e, nullptr);
    if (pHeal.exchange(false) && GM_FullHeal) GM_FullHeal(self, nullptr);
    applySpeed();
    o_GM_FU(self, mi);
}

// ===== hook generik: semua register argumen diteruskan apa adanya =====
typedef void (*G8)(void *, long, long, long, long, long, long, long, float, float, float, float, float, float, float, float);
#define GARGS void *s, long a1, long a2, long a3, long a4, long a5, long a6, long a7, float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7
#define GPASS s, a1, a2, a3, a4, a5, a6, a7, f0, f1, f2, f3, f4, f5, f6, f7

// kerusakan yang diterima pemain: god mode + defense multiplier
#define PLAYER_HOOK(NAME) \
    static G8 o_##NAME; \
    static void h_##NAME(GARGS) { \
        if (godMode) return; \
        int dm = defMul; \
        if (dm > 1) { a1 = (long)(unsigned)((int)a1 / dm); f0 /= (float)dm; } \
        o_##NAME(GPASS); \
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

// buka semua sayap & aksesori (eksperimental)
#define HAVE_HOOK(NAME) \
    static bool (*o_##NAME)(void *, int, const void *); \
    static bool h_##NAME(void *s, int i, const void *m) { \
        if (unlockAll) return true; \
        return o_##NAME(s, i, m); \
    }
HAVE_HOOK(AccHave)
HAVE_HOOK(WingHave)
HAVE_HOOK(MySetWing)
HAVE_HOOK(MySetAcc)

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
    resolveIcalls(base);
    GM_UpdateGold = (decltype(GM_UpdateGold))(base + RVA_GM_UpdateGold);
    GM_UpdateCrystal = (decltype(GM_UpdateCrystal))(base + RVA_GM_UpdateCrystal);
    GM_UpdateExp = (decltype(GM_UpdateExp))(base + RVA_GM_UpdateExp);
    GM_FullHeal = (decltype(GM_FullHeal))(base + RVA_GM_FullHeal);

    hk(base, RVA_GM_FixedUpdate, (void *)h_GM_FU, (void **)&o_GM_FU, "GM_FixedUpdate");
    hk(base, RVA_PC_Damege, (void *)h_PC_Damege, (void **)&o_PC_Damege, "PC_Damege");
    hk(base, RVA_PC_AbsDamege, (void *)h_PC_AbsDamege, (void **)&o_PC_AbsDamege, "PC_AbsDamege");
    hk(base, RVA_PC_UserDamege, (void *)h_PC_UserDamege, (void **)&o_PC_UserDamege, "PC_UserDamege");
    hk(base, RVA_DDD_Damege, (void *)h_DDD_Damege, (void **)&o_DDD_Damege, "DDD_Damege");
    hk(base, RVA_MMC_Damege, (void *)h_MMC, (void **)&o_MMC, "MMC");
    hk(base, RVA_ASMC_Damege, (void *)h_ASMC, (void **)&o_ASMC, "ASMC");
    hk(base, RVA_ABMC_Damege, (void *)h_ABMC, (void **)&o_ABMC, "ABMC");
    hk(base, RVA_AMC_Damege, (void *)h_AMC, (void **)&o_AMC, "AMC");
    hk(base, RVA_DDDASMC_Damege, (void *)h_DDDASMC, (void **)&o_DDDASMC, "DDDASMC");
    hk(base, RVA_GM_AccHave, (void *)h_AccHave, (void **)&o_AccHave, "AccHave");
    hk(base, RVA_GM_WingHave, (void *)h_WingHave, (void **)&o_WingHave, "WingHave");
    hk(base, RVA_GM_MySetWing, (void *)h_MySetWing, (void **)&o_MySetWing, "MySetWing");
    hk(base, RVA_GM_MySetAcc, (void *)h_MySetAcc, (void **)&o_MySetAcc, "MySetAcc");
    g_loaded = true;
    LOGI("semua hook terpasang");
    return nullptr;
}

// ===== JNI (kontrak dengan menu Java bawaan) =====
static const char *kIcon = "iVBORw0KGgoAAAANSUhEUgAAAGAAAABgCAYAAADimHc4AAADT0lEQVR42u2du1LDMBBF0Q4VddySvyENn0sDfxNaXFNTZcZj/JD2Ie1KdysYEiLfs3vXsmPp6QmBGDlShEG+Xt5+OO/7nr8mAKgkdlQoaQTRPcNIIwrvCUSKIvr94/dS8vrr+8scAUbyKHyp2BZQaoFIXsS3El0CowaE1FL42qJzYViCSC3E5whf6unan2EFIXkUnttAtaDUBJG8iG8lusV4NCEka/G9Ca81Pi0IqZX4rYXXGKsGhFRbfG/CS8cthZC0xY+U9VrVIIGQaogfRXjJ8XAhUHTx7z+fZpO5vbFvHSv3Ghch89tCoMjiP7LfsgqsIVDNAfdYCdIg7ezvUfyjY5NWAUH8thBYFuRB/LXvW/cBDgQVADkkR8h8zjHnaEeaM93Rg2NFJP2AEbNf04ooovXs+X3NPqBlRSTJfoRcK7Ikj4bMBLAuGU/Zf2YzLWwopwr2bIiQ/W2rgCTTaERZbGlLpaWE7M+vghzrpkgHmOvvXvpAsQXBfurbEMF+2toQQTJHFtSD/0frA6gALwDQgNs0YorQgLl20sqGShoxLAg9AAAQAGDn495PR1EBAAAACACw9W/PfQAVAAAA0LX9eLehXQCc+5uIba2OrqM9P374nr+maFdEr9NtjjLhWsbyicqwPWAp/tbv6AFGfn2dbvOe2Ed/81olFDnrpa9zD8BLIz7LbK331G7A/wB4XGlWKqK3alhr7M6CHj6tmcHL/+WtDzyfDv79ZV6W1f3j92J9f3hLeI014K7TbbYEwLmPTmclEs1yPFvSlrZqzwlj9qv4nPCaFL4Tyqi0lWZ7zkK1iSP7mQBQBfzsZwHIacaogjwNjrQ8XDMOq6SUi1+6nhyVkoMVlVnPmZOQpHxGtiKp9YjOgjTXy+nZetTOgnKtaAQIGr7PqgBA0BdfNBEbrSdYHVMRgJKzop4gWC5hXFwBo0GwXj+aZUGjQKixeDeWry9MGjfL1x9BKM2eqFkvFV8FAAeCRxBhtzCRQPAAootNfM4geATR3TZWGhCsYXS/kZs2CAmUYbcyzIXgaY7Q5WaeJSBawBhiO9tSCNYwht3QmQtCAgVbmhvC0I6WX8dMHgRoBcLD1/FdAKgJw9szEO4AaELx+MAJAoFYxh+7B8iCUmRTrAAAAABJRU5ErkJggg==";

static jobjectArray makeArray(JNIEnv *env, const char **arr, int n) {
    jclass sc = env->FindClass("java/lang/String");
    jobjectArray ret = env->NewObjectArray(n, sc, env->NewStringUTF(""));
    for (int i = 0; i < n; i++) env->SetObjectArrayElement(ret, i, env->NewStringUTF(arr[i]));
    return ret;
}

static jobjectArray JNICALL GetFeatureList(JNIEnv *env, jobject) {
    static const char *f[] = {
        "0_Toggle_God Mode",
        "1_SeekBar_Damage Multiplier_1_100",
        "2_SeekBar_Defense Multiplier_1_100",
        "3_InputValue_2000000000_Add Gold",
        "4_InputValue_2000000000_Add Crystal",
        "5_InputValue_2000000000_Add EXP",
        "6_Toggle_One Hit Kill",
        "7_SeekBar_Game Speed_1_5",
        "8_Button_Full Heal",
        "9_Toggle_Unlock Wings & Accessories (Experimental)"};
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
    setHtml(env, subtitle, "<b>God Mode - Multiplier - Add Gold/Crystal/EXP</b>");
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

static void addTo(std::atomic<int> &t, int v) {
    if (v <= 0) return;
    if (time(nullptr) - g_start < 10) return; // abaikan pemulihan pengaturan saat game baru dibuka
    long long n = (long long)t.load() + v;
    if (n > 2000000000LL) n = 2000000000LL;
    t.store((int)n);
}

static void JNICALL Changes(JNIEnv *env, jobject, jobject ctx, jint feat, jstring name, jint value, jboolean b, jstring str) {
    switch (feat) {
        case 0: godMode = b; break;
        case 1: dmgMul = value < 1 ? 1 : value; break;
        case 2: defMul = value < 1 ? 1 : value; break;
        case 3: addTo(pGold, value); break;
        case 4: addTo(pCrystal, value); break;
        case 5: addTo(pExp, value); break;
        case 6: oneHit = b; break;
        case 7: gameSpeed = value < 1 ? 1 : value; break;
        case 8: pHeal = true; break;
        case 9: unlockAll = b; break;
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
