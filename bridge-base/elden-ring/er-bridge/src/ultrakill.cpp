// Host side of the ULTRAKILL Crossover Bridge's optional protocol extensions
// (include/bridge_protocol_ext.h, the kit's docs/protocol.md section 8). Minecraft never writes
// the guest request block, so with Minecraft nothing here changes behaviour: every feature
// waits for an ULTRAKILL guest that publishes it.
//
//   host events   weapon progression (bosses defeated = the game's global boss flags
//                 9100-9299, run seed = the character), and NEEDS_INPUT while a menu that
//                 blocks Elden Ring's actions is open (the guest hands input over meanwhile)
//   host combat   health (Elden Ring owns V1's health: game.cpp stand_in), stat ratios
//                 (Dexterity, FP, stamina) and the damage stat (best attack rating) for
//                 ULTRAKILL's stat damage entries
//   guest requests  heals, i-frames and parry windows from V1 (read by game.cpp)
//
// Layouts are fromsoftware-rs's for eldenring.exe 2.7.2.0 (PlayerGameData, CSEventFlagMan,
// CSMenuManImp); every read goes through ReadProcessMemory and is checked for plausibility.
// Settings: <bridge dir>\er-bridge.ini, [ULTRAKILL] (written with defaults on first run).
#include "common.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

namespace mb {

// ---------------------------------------------------------------------------------------
// Settings

static struct {
    bool hostHealth = true;     // Elden Ring owns V1's health
    float healScale = 0.5f;     // Elden Ring HP per ULTRAKILL heal, as a share of max HP per 100 UK HP
    bool stats = true;          // attributes drive V1 (attack speed, recharge)
    bool statDamage = true;     // V1's damage follows the attack rating
    float damageScale = 0.2f;   // Elden Ring HP per (attack rating x ULTRAKILL damage x weapon k)
    uint32_t loadout = ERMC_LOADOUT_PROGRESSION;
    bool menuInput = true;      // hand input to Elden Ring while one of its menus is open
} g_cfg;

static const char kIniTemplate[] =
    "; ULTRAKILL guest options for the Elden Ring host (erbridge_core.dll).\n"
    "; Read when the game starts. Delete this file to get the defaults back.\n"
    "[ULTRAKILL]\n"
    "; Elden Ring's HP is V1's health: Elden Ring's damage, armour and flasks count, V1's dash\n"
    "; and hurt i-frames dodge hits, a punch parries, ULTRAKILL heals heal the Tarnished.\n"
    "HostHealth=1\n"
    "; Elden Ring HP healed per 100 ULTRAKILL HP of healing, as a share of max HP.\n"
    "HealScale=0.5\n"
    "; Dexterity speeds up V1's weapons, max FP its alt-fire recharge, max stamina its dashes.\n"
    "Stats=1\n"
    "; V1's damage follows your best weapon's attack rating (off: a fixed rate set by the guest).\n"
    "StatDamage=1\n"
    "; Elden Ring HP per point of attack rating per ULTRAKILL damage point (x weapon factor).\n"
    "DamageScale=0.2\n"
    "; Progression: one weapon at the start, one more per boss defeated. All: every weapon.\n"
    "; Guest: the guest's own [Loadout] setting decides.\n"
    "Loadout=Progression\n"
    "; While a menu needs Elden Ring's controls (Site of Grace, talking, item pop-ups), control\n"
    "; goes to Elden Ring and comes back when it closes.\n"
    "MenuInput=1\n";

static float ini_float(const char* path, const char* key, float def) {
    char buf[64];
    char defs[32];
    snprintf(defs, sizeof(defs), "%g", def);
    GetPrivateProfileStringA("ULTRAKILL", key, defs, buf, sizeof(buf), path);
    char* end = nullptr;
    float v = strtof(buf, &end);
    return end != buf && isfinite(v) ? v : def;
}

static void load_config() {
    char path[MAX_PATH * 4];
    snprintf(path, sizeof(path), "%s", bridge_path("er-bridge.ini"));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(path, "w");
        if (f) {
            fputs(kIniTemplate, f);
            fclose(f);
        }
    }
    g_cfg.hostHealth = GetPrivateProfileIntA("ULTRAKILL", "HostHealth", 1, path) != 0;
    g_cfg.healScale = fminf(fmaxf(ini_float(path, "HealScale", 0.5f), 0.0f), 2.0f);
    g_cfg.stats = GetPrivateProfileIntA("ULTRAKILL", "Stats", 1, path) != 0;
    g_cfg.statDamage = GetPrivateProfileIntA("ULTRAKILL", "StatDamage", 1, path) != 0;
    g_cfg.damageScale = fminf(fmaxf(ini_float(path, "DamageScale", 0.2f), 0.01f), 10.0f);
    g_cfg.menuInput = GetPrivateProfileIntA("ULTRAKILL", "MenuInput", 1, path) != 0;
    char lo[32];
    GetPrivateProfileStringA("ULTRAKILL", "Loadout", "Progression", lo, sizeof(lo), path);
    g_cfg.loadout = !_stricmp(lo, "All") ? ERMC_LOADOUT_ALL : !_stricmp(lo, "Guest") ? ERMC_LOADOUT_GUEST : ERMC_LOADOUT_PROGRESSION;
    log("ultrakill: settings: health %s (heal %.2f), stats %s, stat damage %s (scale %.2f), loadout %s, menu input %s",
        g_cfg.hostHealth ? "Elden Ring" : "V1", g_cfg.healScale, g_cfg.stats ? "on" : "off", g_cfg.statDamage ? "on" : "off",
        g_cfg.damageScale, g_cfg.loadout == ERMC_LOADOUT_ALL ? "all" : g_cfg.loadout == ERMC_LOADOUT_GUEST ? "guest" : "progression",
        g_cfg.menuInput ? "on" : "off");
}

// ---------------------------------------------------------------------------------------
// Statics

static uintptr_t g_base = 0;
static uintptr_t g_eventFlagManRva = 0, g_menuManRva = 0;

// "mov reg, [rip+X]" at a unique code signature -> the static's RVA.
static uintptr_t rip_static(const char* sig, int dispAt, int insLen, const char* name) {
    uintptr_t hit = find_pattern(sig);
    if (!hit) {
        log("ultrakill: %s not found", name);
        return 0;
    }
    int32_t disp = 0;
    if (!mem_read((void*)(hit + dispAt), &disp, 4)) return 0;
    return hit + insLen + disp - g_base;
}

static uint8_t* read_ptr(uintptr_t rva) {
    uint8_t* p = nullptr;
    if (!rva || !mem_read((void*)(g_base + rva), &p, 8)) return nullptr;
    return p;
}


static void fill_weapon_table();

void uk_init(uintptr_t base, const UkBuild& b) {
    g_base = base;
    load_config();
    fill_weapon_table();
    g_eventFlagManRva = b.eventFlagMan ? b.eventFlagMan
                                       : rip_static("48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9", 3, 7, "CSEventFlagMan");
    g_menuManRva = b.menuMan ? b.menuMan
                             : rip_static("48 8B 0D ?? ?? ?? ?? 48 8B 53 08 48 8B 92 D8 00 00 00 48 83 C4 20 5B", 3, 7, "CSMenuMan");
    log("ultrakill: CSEventFlagMan static %p, CSMenuMan static %p", (void*)(g_eventFlagManRva ? base + g_eventFlagManRva : 0),
        (void*)(g_menuManRva ? base + g_menuManRva : 0));
}

// ---------------------------------------------------------------------------------------
// Event flags (CSFD4VirtualMemoryFlag): flag id / divisor = group; a red-black tree (MSVC
// std::map: left +0, parent +8, right +0x10, isnil +0x19, key +0x20) maps groups to 125-byte
// blocks; descriptor at +0x28: mode 1 = holder index (+0x30) into the holder array, mode 2 =
// pointer (+0x30). Bits are MSB first. Same walk as the game's own getter.

static const uint8_t* flag_group(uint32_t group, uint32_t* divisor, uint8_t* bytes) {
    uint8_t* m = read_ptr(g_eventFlagManRva);
    if (!m) return nullptr;
    uint8_t hdr[0x48];
    if (!mem_read(m, hdr, sizeof(hdr))) return nullptr;
    uint32_t div = *(uint32_t*)(hdr + 0x1C), holderSize = *(uint32_t*)(hdr + 0x20);
    uint8_t* holders = *(uint8_t**)(hdr + 0x28);
    uint8_t* head = *(uint8_t**)(hdr + 0x38);
    if (div == 0 || div > 100000 || !head) return nullptr;
    *divisor = div;
    uint8_t* node = nullptr;
    if (!mem_read(head + 8, &node, 8)) return nullptr;
    uint8_t* best = head;
    for (int depth = 0; depth < 64 && node; depth++) {
        uint8_t nb[0x38];
        if (!mem_read(node, nb, sizeof(nb)) || nb[0x19]) break;
        if (*(uint32_t*)(nb + 0x20) < group) {
            node = *(uint8_t**)(nb + 0x10);
        } else {
            best = node;
            node = *(uint8_t**)(nb + 0x00);
        }
    }
    uint8_t bb[0x38];
    if (best == head || !mem_read(best, bb, sizeof(bb)) || *(uint32_t*)(bb + 0x20) != group) return nullptr;
    uint32_t mode = *(uint32_t*)(bb + 0x28);
    const uint8_t* block = mode == 1 ? holders + (size_t)*(uint32_t*)(bb + 0x30) * holderSize
                         : mode == 2 ? *(uint8_t**)(bb + 0x30)
                                     : nullptr;
    if (!block || !mem_read(block, bytes, 125)) return nullptr;
    return block;
}

// Global boss-defeated flags (EventFlagUsageParam "ボス撃破"): 9100-9135 legacy dungeon and
// major bosses, 9170-9184 divine towers and forts, 9200-9269 catacombs, caves and tunnels.
static bool count_bosses(uint32_t* out) {
    uint32_t div = 0;
    uint8_t bytes[125];
    if (!flag_group(9100 / 1000, &div, bytes) || div != 1000) return false;
    uint32_t n = 0;
    for (uint32_t id = 9100; id < 9300; id++) {
        uint32_t r = id % 1000;
        if (bytes[r >> 3] & (0x80u >> (r & 7))) n++;
    }
    *out = n;
    return true;
}

// ---------------------------------------------------------------------------------------
// PlayerGameData (PlayerIns +0x580)

constexpr size_t kPlayerGameData = 0x580;
constexpr size_t kPgdMaxFp = 0x20;            // current max FP (with buffs)
constexpr size_t kPgdMaxStamina = 0x30;
constexpr size_t kPgdAttrs = 0x3C;            // vigor, mind, endurance, strength, dexterity, intelligence, faith, arcane
constexpr size_t kPgdLevel = 0x68;
constexpr size_t kPgdName = 0x9C;             // wchar_t[17]
constexpr size_t kPgdArchetype = 0xBF;
constexpr size_t kPgdEffDex = 0x29C;          // dexterity after buffs and corrections
constexpr size_t kPgdAttackRating = 0x968;    // i32 x6: left/right primary, secondary, tertiary
constexpr size_t kPgdRead = 0x980;
// Normalisation: a character with 10 in every attribute (about 78 FP and 96 stamina) is x1.0.
constexpr float kBaseFp = 78.0f, kBaseStamina = 96.0f, kBaseDex = 10.0f;

struct PlayerStats {
    bool valid;
    uint32_t level;
    float attackRating;    // best of the six weapon slots
    float attackSpeed, rechargeFp, rechargeStamina;
    uint64_t seed;
};
static PlayerStats g_stats = {};

static bool read_stats(uint8_t* playerIns, PlayerStats* s) {
    static uint8_t buf[kPgdRead];
    uint8_t* pgd = nullptr;
    if (!playerIns || !mem_read(playerIns + kPlayerGameData, &pgd, 8) || !pgd) return false;
    if (!mem_read(pgd, buf, sizeof(buf))) return false;
    const uint32_t* a = (const uint32_t*)(buf + kPgdAttrs);
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        if (a[i] < 1 || a[i] > 99) return false;
        sum += a[i];
    }
    uint32_t level = *(uint32_t*)(buf + kPgdLevel);
    if (level < 1 || level > 713 || level + 79 != sum) return false;  // level = attributes - 79
    s->level = level;
    int32_t best = 0;
    for (int i = 0; i < 6; i++) {
        int32_t ar = *(int32_t*)(buf + kPgdAttackRating + 4 * i);
        if (ar > best && ar < 10000) best = ar;
    }
    s->attackRating = (float)best;
    uint32_t dex = *(uint32_t*)(buf + kPgdEffDex);
    if (dex < 1 || dex > 150) dex = a[4];
    float fp = (float)*(uint32_t*)(buf + kPgdMaxFp), st = (float)*(uint32_t*)(buf + kPgdMaxStamina);
    s->attackSpeed = 1.0f + ((float)dex - kBaseDex) * 0.01f;
    s->rechargeFp = fp > 0 && fp < 2000 ? fp / kBaseFp : 1.0f;
    s->rechargeStamina = st > 0 && st < 1000 ? st / kBaseStamina : 1.0f;
    // The run seed: the character's name and origin (a new character starts a new progression order).
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < 17 * 2; i++) h = (h ^ buf[kPgdName + i]) * 1099511628211ull;
    h = (h ^ buf[kPgdArchetype]) * 1099511628211ull;
    s->seed = h ? h : 1;
    s->valid = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// Menus that block Elden Ring's actions: the action-button manager clears its "can execute"
// byte while CSMenuManImp.ui_states[0x1C] or [0x3D] has its "created" bit (talks, the Site of
// Grace menu, item pop-ups...). The same test, debounced, asks the guest for host input.

// The game only tests them while the u32 at +0x1C is nonzero (ActionButtonMan update).
constexpr size_t kMenuGate = 0x1C;
constexpr size_t kMenuUiStates = 0x90;
constexpr size_t kMenuUiCount = 0x46;

static bool read_ui_states(uint8_t* out, bool* gate) {
    uint8_t* m = read_ptr(g_menuManRva);
    uint8_t buf[kMenuUiStates + kMenuUiCount];
    if (!m || !mem_read(m, buf, sizeof(buf))) return false;
    *gate = *(uint32_t*)(buf + kMenuGate) != 0;
    memcpy(out, buf + kMenuUiStates, kMenuUiCount);
    return true;
}

// ---------------------------------------------------------------------------------------
// Guest requests

static ErmcGuestRequests g_req = {};
static bool g_present = false;
static uint64_t g_heartbeat = 0, g_heartbeatMs = 0;
static bool g_reqBase = false;
static uint32_t g_healSeen = 0, g_punchSeen = 0;

static bool read_requests(ErmcGuestRequests* out) {
    ErmcGuestRequests* p = shm_guest_requests();
    for (int tries = 0; tries < 64; tries++) {
        uint32_t s1 = p->seq;
        __asm__ __volatile__("" ::: "memory");
        if (s1 & 1) {
            YieldProcessor();
            continue;
        }
        memcpy(out, (const void*)p, sizeof(*out));
        __asm__ __volatile__("" ::: "memory");
        if (p->seq == s1) return out->magic == ERMC_GREQ_MAGIC && out->version == ERMC_GREQ_VERSION;
    }
    return false;
}

void uk_tick_begin() {
    uint64_t now = now_ms();
    uint64_t hb = shm_header()->mcHeartbeat;
    if (hb != g_heartbeat) {
        g_heartbeat = hb;
        g_heartbeatMs = now;
    }
    ErmcGuestRequests r;
    bool ok = read_requests(&r) && (r.extFlags & ERMC_GREQ_EXT_COMBAT_VALID);
    if (ok) g_req = r;
    bool present = (ok || g_present) && now - g_heartbeatMs < 2000 && g_req.magic == ERMC_GREQ_MAGIC;
    if (present != g_present) {
        log("ultrakill: %s", present ? "ULTRAKILL guest detected (extensions on)" : "ULTRAKILL guest gone (extensions off)");
        g_present = present;
    }
    if (!present) {
        g_reqBase = false;
        return;
    }
    if (!g_reqBase) {
        g_reqBase = true;
        g_healSeen = g_req.healMilli;
        g_punchSeen = g_req.punchSeq;
    }
}

bool uk_guest_present() { return g_present; }
bool uk_owns_health() { return g_present && g_cfg.hostHealth; }
uint32_t uk_guest_combat_flags() { return g_present ? g_req.combatFlags : 0; }

float uk_take_heal(int maxHp) {
    if (!g_present || !g_reqBase) return 0;
    uint32_t d = g_req.healMilli - g_healSeen;
    g_healSeen = g_req.healMilli;
    if (d == 0 || d > 10000000u) return 0;  // > 10000 UK HP at once: a restarted counter
    return (float)d / 1000.0f / 100.0f * (float)maxHp * g_cfg.healScale;
}

bool uk_take_parry() {
    if (!g_present || !g_reqBase || g_req.punchSeq == g_punchSeen) return false;
    g_punchSeen = g_req.punchSeq;
    return true;
}

// ---------------------------------------------------------------------------------------
// Stat damage: weapon factors (the kit's WeaponTable defaults, host SDK StatDamage.cs)

static float g_k[64], g_proc[64];

static void fill_weapon_table() {
    for (int i = 0; i < 64; i++) g_k[i] = 1.0f, g_proc[i] = 0.25f;
    auto derived = [](float hitsPerSec, int pellets) {
        float v = 3.0f / hitsPerSec;  // ProcBudget.PerShot: 3 procs/s, clamped to [0.05, 1.5]
        v = v < 0.05f ? 0.05f : v > 1.5f ? 1.5f : v;
        return v / (float)pellets;
    };
    struct Row { int id; float k; float proc; };
    const Row rows[] = {
        {ERMC_WEAPON_UNKNOWN, 1.00f, 0.25f},       {ERMC_WEAPON_REV_SHOT, 2.00f, derived(2.0f, 1)},
        {ERMC_WEAPON_REV_PIERCER, 3.00f, 1.0f},    {ERMC_WEAPON_REV_MARKSMAN, 2.67f, 1.0f},
        {ERMC_WEAPON_COIN_HIT, 1.00f, 1.0f},       {ERMC_WEAPON_SHO_PELLET, 0.30f, derived(1.25f, 12)},
        {ERMC_WEAPON_SHO_ZONE, 0.15f, 0.25f},      {ERMC_WEAPON_SHO_OVERCHARGE, 1.00f, 1.0f},
        {ERMC_WEAPON_SHO_GRENADE, 0.80f, 1.0f},    {ERMC_WEAPON_SHO_SAW, 1.00f, 0.2f},
        {ERMC_WEAPON_HAMMER, 0.50f, derived(0.8f, 1)}, {ERMC_WEAPON_NAIL, 1.65f, derived(12.0f, 1)},
        {ERMC_WEAPON_NAIL_BURST, 1.65f, 1.0f},     {ERMC_WEAPON_SAWBLADE, 2.00f, 0.5f},
        {ERMC_WEAPON_ZAPPER, 1.00f, 0.2f},         {ERMC_WEAPON_RAIL_BEAM, 3.75f, 1.0f},
        {ERMC_WEAPON_RAIL_MALICIOUS, 3.12f, 1.0f}, {ERMC_WEAPON_RAIL_HARPOON, 2.50f, 0.2f},
        {ERMC_WEAPON_ROCKET, 0.56f, 1.0f},         {ERMC_WEAPON_CANNONBALL, 0.90f, 1.0f},
        {ERMC_WEAPON_NAPALM, 1.00f, 0.1f},         {ERMC_WEAPON_PUNCH, 2.50f, derived(1.25f, 1)},
        {ERMC_WEAPON_KNUCKLE, 2.60f, derived(0.83f, 1)}, {ERMC_WEAPON_WHIP, 2.00f, 0.5f},
        {ERMC_WEAPON_SLAM, 2.00f, 1.0f},           {ERMC_WEAPON_PARRY, 15.0f, 1.0f},
        {ERMC_WEAPON_EXPLOSION_OTHER, 0.80f, 0.5f}, {ERMC_WEAPON_FIRE_OTHER, 1.00f, 0.05f},
    };
    for (const Row& r : rows) g_k[r.id] = r.k, g_proc[r.id] = r.proc;
}

constexpr float kHeadshot = 1.5f;
static float g_damageStat = 0;  // published damage stat (0: STAT entries not advertised)

float uk_hit_damage(const ErmcDamage& d) {
    if (!(d.flags & ERMC_DAMAGE_STAT) || (d.flags & ERMC_DAMAGE_FRACTION)) return -1.0f;
    float dmg = (g_damageStat > 0 ? g_damageStat : g_stats.attackRating > 0 ? g_stats.attackRating : 100.0f) *
                g_cfg.damageScale * g_k[d.reserved & 63] * d.amount;
    if (d.flags & ERMC_DAMAGE_WEAKPOINT) dmg *= kHeadshot;
    return dmg;
}

// ---------------------------------------------------------------------------------------
// Publishing (seqlocks; magic written last, as the host SDK does)

static void seq_begin(volatile uint32_t* s) {
    uint32_t v = *s;
    if (v & 1) v++;
    *s = v + 1;
    __asm__ __volatile__("" ::: "memory");
}
static void seq_end(volatile uint32_t* s) {
    __asm__ __volatile__("" ::: "memory");
    uint32_t v = *s + 1;
    *s = v ? v : 2;
}

static void write_combat(const ErmcHostCombat& want) {
    ErmcHostCombat* p = shm_host_combat();
    // Everything after seq (0x0C onwards) decides whether to write.
    if (p->magic == ERMC_HOSTCOMBAT_MAGIC && p->version == ERMC_HOSTCOMBAT_VERSION &&
        !memcmp((const uint8_t*)p + 0x0C, (const uint8_t*)&want + 0x0C, sizeof(want) - 0x0C))
        return;
    seq_begin(&p->seq);
    p->version = ERMC_HOSTCOMBAT_VERSION;
    memcpy((uint8_t*)p + 0x0C, (const uint8_t*)&want + 0x0C, sizeof(want) - 0x0C);
    __asm__ __volatile__("" ::: "memory");
    p->magic = ERMC_HOSTCOMBAT_MAGIC;
    seq_end(&p->seq);
}

static void write_events(uint32_t flags, uint32_t mode, uint64_t seed, const uint32_t* counters) {
    ErmcHostEvents* p = shm_host_events();
    if (p->magic == ERMC_HOSTEV_MAGIC && p->version == ERMC_HOSTEV_VERSION && p->flags == flags && p->loadoutMode == mode &&
        p->runSeed == seed && !memcmp(p->counters, counters, sizeof(p->counters)))
        return;
    seq_begin(&p->seq);
    p->version = ERMC_HOSTEV_VERSION;
    p->flags = flags;
    p->loadoutMode = mode;
    p->reserved0 = 0;
    p->runSeed = seed;
    memcpy(p->counters, counters, sizeof(p->counters));
    __asm__ __volatile__("" ::: "memory");
    p->magic = ERMC_HOSTEV_MAGIC;
    seq_end(&p->seq);
}

static uint32_t g_tick = 0;
static uint64_t g_seed = 0;
static uint32_t g_bosses = 0;
static bool g_bossesKnown = false;
static bool g_statsLogged = false;
static PlayerStats g_loggedStats = {};
static bool g_menuOpen = false, g_menuRaw = false;
static uint64_t g_menuRawSince = 0;
static uint8_t g_lastUi[kMenuUiCount];
static bool g_haveLastUi = false;

static void update_menu(bool alive) {
    uint8_t ui[kMenuUiCount];
    bool gate = false;
    bool haveUi = alive && read_ui_states(ui, &gate);
    bool raw = haveUi && gate && g_cfg.menuInput && ((ui[0x1C] | ui[0x3D]) & 1);
    if (haveUi) {
        // Diagnostics: which UI elements exist (helps map menus to indices).
        static int logged = 0;
        if (g_haveLastUi && logged < 300) {
            for (size_t i = 0; i < kMenuUiCount; i++) {
                if (((ui[i] ^ g_lastUi[i]) & 1) && logged++ < 300)
                    log("ultrakill: ui %#x %s", (unsigned)i, (ui[i] & 1) ? "created" : "gone");
            }
        }
        memcpy(g_lastUi, ui, sizeof(ui));
        g_haveLastUi = true;
    }
    uint64_t now = now_ms();
    if (raw != g_menuRaw) {
        g_menuRaw = raw;
        g_menuRawSince = now;
    }
    // On after 150 ms, off after 400 ms: a flickering menu must not bounce control between the games.
    if (raw != g_menuOpen && now - g_menuRawSince >= (raw ? 150u : 400u)) {
        g_menuOpen = raw;
        log("ultrakill: Elden Ring menu %s", raw ? "open -> asking ULTRAKILL for Elden Ring input" : "closed");
    }
}

void uk_publish(const UkTick& t) {
    g_tick++;
    if (t.alive && t.playerIns && (g_tick % 15 == 0 || !g_stats.valid)) {
        PlayerStats s = {};
        if (read_stats(t.playerIns, &s)) {
            g_stats = s;
            g_seed = s.seed;
            if (!g_statsLogged || s.level != g_loggedStats.level || s.attackRating != g_loggedStats.attackRating ||
                s.attackSpeed != g_loggedStats.attackSpeed || s.rechargeFp != g_loggedStats.rechargeFp ||
                s.rechargeStamina != g_loggedStats.rechargeStamina) {
                log("ultrakill: level %u, attack rating %.0f, attack speed x%.2f, FP recharge x%.2f, stamina recharge x%.2f",
                    s.level, s.attackRating, s.attackSpeed, s.rechargeFp, s.rechargeStamina);
                g_loggedStats = s;
                g_statsLogged = true;
            }
        } else if (!g_stats.valid && g_tick % 600 == 0) {
            log("ultrakill: PlayerGameData not readable or implausible; stats and stat damage stay off");
        }
    }
    if (t.alive && g_tick % 30 == 0) {
        uint32_t n;
        if (count_bosses(&n)) {
            if (!g_bossesKnown || n != g_bosses) log("ultrakill: %u boss%s defeated", n, n == 1 ? "" : "es");
            g_bosses = n;
            g_bossesKnown = true;
        } else if (!g_bossesKnown && g_tick % 600 == 0) {
            log("ultrakill: boss flags not readable; weapon progression waits");
        }
    }
    update_menu(t.alive);

    ErmcHostCombat c;
    memset(&c, 0, sizeof(c));
    uint32_t flags = 0;
    if (g_stats.valid) {
        c.flags |= ERMC_COMBAT_STATS_VALID;
        c.level = g_stats.level;
        c.damage = g_stats.attackRating > 0 ? g_stats.attackRating : 1.0f;
        c.critPercent = 0;
        c.critMultiplier = 1.0f;
        c.damageScale = g_cfg.damageScale;
        c.headshotMultiplier = kHeadshot;
        for (int i = 0; i < 64; i++) c.weapons[i].k = g_k[i], c.weapons[i].proc = g_proc[i];
        if (g_cfg.statDamage && g_stats.attackRating > 0) flags |= ERMC_HOSTFLAG_STAT_DAMAGE;
        if (g_cfg.stats) {
            auto clamp = [](float v) { return !isfinite(v) ? 1.0f : v < 0 ? 0.0f : v > 10.0f ? 10.0f : v; };
            c.flags |= ERMC_COMBAT_RATIOS_VALID;
            c.attackSpeedRatio = clamp(g_stats.attackSpeed);
            c.moveSpeedRatio = 1.0f;
            c.extraJumps = 0;
            c.jumpPowerRatio = 1.0f;
            c.sprintSpeedRatio = 1.0f;
            c.rechargeSecondary = clamp(g_stats.rechargeFp);
            c.rechargeSpecial = clamp(g_stats.rechargeFp);
            c.rechargeUtility = clamp(g_stats.rechargeStamina);
            flags |= ERMC_HOSTFLAG_STATS;
        }
    }
    g_damageStat = (flags & ERMC_HOSTFLAG_STAT_DAMAGE) ? c.damage : 0;
    if (uk_owns_health() && t.playerIns && t.maxHp > 0) {
        c.flags |= ERMC_COMBAT_HEALTH_VALID;
        c.health = (float)(t.hp > 0 ? t.hp : 0);
        c.fullHealth = (float)t.maxHp;
        if (t.dead) c.flags |= ERMC_COMBAT_DEAD;
        flags |= ERMC_HOSTFLAG_OWNS_HEALTH;
    } else if (uk_owns_health()) {
        // No character this tick (loading): keep the last health instead of flapping the guest's model.
        const ErmcHostCombat* p = shm_host_combat();
        if (p->magic == ERMC_HOSTCOMBAT_MAGIC && (p->flags & ERMC_COMBAT_HEALTH_VALID)) {
            c.flags |= p->flags & (ERMC_COMBAT_HEALTH_VALID | ERMC_COMBAT_DEAD);
            c.health = p->health;
            c.fullHealth = p->fullHealth;
            flags |= ERMC_HOSTFLAG_OWNS_HEALTH;
        }
    }
    if (g_menuOpen) flags |= ERMC_HOSTFLAG_NEEDS_INPUT;
    if (!g_present) flags = 0;
    write_combat(c);  // before the flags that advertise it
    uint32_t counters[ERMC_HOSTEV_COUNTERS] = {};
    counters[ERMC_CTR_BOSSES_DEFEATED] = g_bosses;
    uint32_t mode = g_cfg.loadout;
    if (mode == ERMC_LOADOUT_PROGRESSION && (!g_bossesKnown || !g_seed)) mode = ERMC_LOADOUT_GUEST;  // not known yet
    write_events(flags, mode, g_seed, counters);
}

void uk_shutdown() {
    // The guest falls back to its own behaviour (V1's health, its own stats and loadout).
    ErmcHostEvents* e = shm_host_events();
    if (e->magic == ERMC_HOSTEV_MAGIC) {
        seq_begin(&e->seq);
        e->flags = 0;
        e->loadoutMode = ERMC_LOADOUT_GUEST;
        seq_end(&e->seq);
    }
    ErmcHostCombat* c = shm_host_combat();
    if (c->magic == ERMC_HOSTCOMBAT_MAGIC) {
        seq_begin(&c->seq);
        c->flags = 0;
        seq_end(&c->seq);
    }
}

}  // namespace mb
