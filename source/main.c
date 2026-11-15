#include <3ds.h>
#include <3ds/services/am.h>
#include <curl/curl.h>
#include <jansson.h>
#include "diagnostics.h"

#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK_COUNT 5
#define API_BUFFER_SIZE (32 * 1024)
#define SOCKET_BUFFER_SIZE (1024 * 1024)
#define MAX_FIRM_SIZE (12 * 1024 * 1024)
#define MAX_SD_TITLE_COUNT 4096
#define CA_PATH "romfs:/ca-bundle.pem"
#define LANGUAGE_CONFIG_PATH "sdmc:/3ds/ShieldChecker/ui-language.dat"

typedef enum {
    STATUS_UNKNOWN = 0,
    STATUS_MISSING,
    STATUS_CURRENT,
    STATUS_UPDATE
} CheckStatus;

typedef enum {
    SCREEN_LANGUAGE = 0,
    SCREEN_WELCOME,
    SCREEN_MENU,
    SCREEN_SCANNING,
    SCREEN_RESULTS,
    SCREEN_DETAILS,
    SCREEN_DIAGNOSTICS,
    SCREEN_DIAG_DETAILS,
    SCREEN_TROUBLE_MENU,
    SCREEN_TROUBLE_DETAILS,
    SCREEN_INPUT_TEST,
    SCREEN_DISPLAY_TEST,
    SCREEN_SETTINGS,
    SCREEN_NETWORK_TEST
} ScreenMode;

typedef enum {
    PATH_ERROR = -1,
    PATH_ABSENT = 0,
    PATH_PRESENT = 1
} PathPresence;

typedef enum {
    TITLE_LOOKUP_UNAVAILABLE = 0,
    TITLE_LOOKUP_ABSENT,
    TITLE_LOOKUP_PRESENT
} TitleLookup;

typedef enum {
    CHECK_LUMA = 0,
    CHECK_GODMODE9,
    CHECK_SEMVER_TITLE,
    CHECK_CIA_VERSION_TITLE,
    CHECK_HSHOP_TITLE
} CheckKind;

typedef struct {
    unsigned part[3];
    unsigned count;
    bool valid;
} Version;

typedef struct {
    const char *name;
    const char *description;
    const char *api_url;
    const char *sd_path;
    CheckKind kind;
    u64 title_id;
    CheckStatus status;
    Version local;
    Version latest;
    char local_text[32];
    char latest_text[32];
    char detected_path[72];
    bool installed;
    bool checked;
    bool sd_file_checked;
    bool sd_file_present;
    bool support_dir_checked;
    bool support_dir_present;
    u16 local_title_version;
    u16 latest_title_version;
    bool latest_title_version_valid;
} CheckEntry;

typedef struct {
    const char *name;
    const char *description;
    const char *api_url;
    const char *sd_path;
    CheckKind kind;
    u64 title_id;
} CheckDefinition;

typedef struct {
    char *data;
    size_t used;
    size_t capacity;
    bool failed;
} HttpBuffer;

static const CheckDefinition check_definitions[CHECK_COUNT] = {
    {"Luma3DS", "Version active et boot.firm sur la SD",
     "https://api.github.com/repos/LumaTeam/Luma3DS/releases/latest",
     "sdmc:/boot.firm", CHECK_LUMA, 0},
    {"GodMode9", "Payload FIRM trouve sur la SD",
     "https://api.github.com/repos/d0k3/GodMode9/releases/latest",
     "sdmc:/luma/payloads/GodMode9.firm", CHECK_GODMODE9, 0},
    {"Universal Updater", "Version installee sur la SD",
     "https://api.github.com/repos/Universal-Team/Universal-Updater/releases/latest",
     NULL, CHECK_SEMVER_TITLE, UINT64_C(0x0004000004391700)},
    {"Shield Updater", "Sur SD ; version publique non lisible",
     "https://api.github.com/repos/Shield-77/Shield-Updater/releases/latest",
     NULL, CHECK_CIA_VERSION_TITLE, UINT64_C(0x000400000A210400)},
    {"hShop / 3hs", "Version du client installee sur la SD",
     "https://download2.erista.me/3hs/version",
     NULL, CHECK_HSHOP_TITLE, UINT64_C(0x0004000003DF1000)}
};

static CheckEntry checks[CHECK_COUNT];

static ScreenMode screen_mode = SCREEN_WELCOME;
static size_t selected_check;
static size_t active_check;
static unsigned scan_step;
static bool service_romfs;
static bool service_soc;
static bool service_am;
static bool sd_title_list_loaded;
static bool sd_title_list_available;
static bool curl_ready;
static u32 *socket_buffer;
static u64 *sd_title_ids;
static u32 sd_title_count;
static char *ca_bundle;
static size_t ca_bundle_size;
static u8 *font_atlas;
static char api_buffer[API_BUFFER_SIZE];
static u64 last_animation;
static unsigned animation_frame;
static const char *scan_phase = "PREPARATION";
static bool ui_french;
static bool language_confirmation_needed;
static DiagnosticReport diagnostic_report;
static size_t selected_menu;
static size_t selected_diagnostic;
static size_t selected_symptom;
static unsigned display_color;
static bool display_lower;
static bool advanced_view;
static ScreenMode test_return_screen = SCREEN_DIAGNOSTICS;
static bool scan_cancelled;
static bool online_requested;
static int last_network_curl_code = -1;
static long last_network_http_status;
static ScreenMode last_result_screen = SCREEN_DIAGNOSTICS;
static bool have_results;

#define MENU_COUNT 5
#define SYMPTOM_COUNT 8
static const char *menu_fr[MENU_COUNT] = {
    "Verification rapide", "Verification avancee", "Aide au diagnostic",
    "Derniers resultats", "Reglages"
};
static const char *menu_en[MENU_COUNT] = {
    "Quick check", "Advanced check", "Troubleshooting",
    "Last results", "Settings"
};
static const char *symptom_fr[SYMPTOM_COUNT] = {
    "Pas de son", "Wi-Fi indisponible", "App ne demarre pas", "SD non detectee",
    "Ralentissements", "Commandes", "Affichage", "Batterie"
};
static const char *symptom_en[SYMPTOM_COUNT] = {
    "No sound", "Wi-Fi unavailable", "App will not start", "SD not detected",
    "Slow performance", "Controls", "Display", "Battery"
};

static const u8 COLOR_BG_R = 4, COLOR_BG_G = 16, COLOR_BG_B = 31;
static const u8 COLOR_PANEL_R = 7, COLOR_PANEL_G = 27, COLOR_PANEL_B = 48;
static const u8 COLOR_CYAN_R = 27, COLOR_CYAN_G = 199, COLOR_CYAN_B = 236;
static const u8 COLOR_WHITE_R = 245, COLOR_WHITE_G = 249, COLOR_WHITE_B = 255;
static const u8 COLOR_MUTED_R = 157, COLOR_MUTED_G = 192, COLOR_MUTED_B = 216;
static const u8 COLOR_GREEN_R = 92, COLOR_GREEN_G = 226, COLOR_GREEN_B = 171;

typedef struct {
    const char *french;
    const char *english;
} UiTranslation;

static const UiTranslation ui_translations[] = {
    {"Version active et boot.firm sur la SD", "Active version and boot.firm on SD"},
    {"Payload FIRM trouve sur la SD", "FIRM payload found on SD"},
    {"Version installee sur la SD", "Installed version on SD"},
    {"Sur SD ; version publique non lisible", "On SD; public version unreadable"},
    {"Version du client installee sur la SD", "Installed client version on SD"},
    {"PREPARATION", "PREPARING"},
    {"RECHERCHE DE LA VERSION", "CHECKING LATEST VERSION"},
    {"LECTURE DE LA SD", "READING SD CARD"},
    {"boot.firm absent de la SD", "boot.firm not found on SD"},
    {"Acces au fichier SD non confirme", "SD file access not confirmed"},
    {"Acces SD non confirme", "SD access not confirmed"},
    {"Payload absent des chemins SD verifies", "Payload not found in checked SD paths"},
    {"Titre non trouve sur la SD", "Title not found on SD"},
    {"Lecture des titres SD indisponible", "SD title list unavailable"},
    {"Titre installe sur la SD", "Title installed on SD"},
    {"STATUT INDISPONIBLE", "STATUS UNAVAILABLE"},
    {"NON INSTALLE SUR SD", "NOT INSTALLED ON SD"},
    {"FICHIER NON TROUVE SUR SD", "FILE NOT FOUND ON SD"},
    {"VERIFIE ET A JOUR", "CHECKED AND UP TO DATE"},
    {"VERSION PLUS RECENTE", "NEWER VERSION AVAILABLE"},
    {"Client non trouve sur la SD.\n", "Client not found on SD.\n"},
    {"La version installee correspond a la\nversion publiee consultee.\n", "Installed version matches the\nlatest version checked.\n"},
    {"Une version publiee est plus recente.\n", "A newer published version is available.\n"},
    {"Comparaison de version indisponible.\n", "Version comparison is unavailable.\n"},
    {"Une version plus recente existe. Cela\n", "A newer version exists. This does\n"},
    {"ne signifie pas qu'il y a un probleme.\n", "not mean there is a problem.\n"},
    {"Luma permet les fonctions personnalisees\n", "Luma enables custom features.\n"},
    {"de la console. Consulte les notes\n", "Check the release notes\n"},
    {"officielles pour voir les changements.\n", "to learn what has changed.\n"},
    {"Sauvegarde la SD avant toute action.\n", "Back up the SD card before any action.\n"},
    {"Checker ne met rien a jour.\n", "Checker does not update anything.\n"},
    {"Une version plus recente est disponible.\n", "A newer version is available.\n"},
    {"GodMode9 aide aux sauvegardes et a\n", "GodMode9 can help with backups and\n"},
    {"certaines taches de maintenance.\n", "some maintenance tasks.\n"},
    {"Il est facultatif selon tes besoins.\n", "It is optional, depending on your needs.\n"},
    {"Universal Updater sert a parcourir\n", "Universal Updater lets you browse\n"},
    {"et installer des homebrews.\n", "and install homebrew apps.\n"},
    {"Shield Updater gere les applications\n", "Shield Updater manages available\n"},
    {"Shield disponibles.\n", "Shield apps.\n"},
    {"Checker ne telecharge ni n'installe.\n", "Checker does not download or install.\n"},
    {"GodMode9.firm n'a pas ete trouve dans\n", "GodMode9.firm was not found in the\n"},
    {"les chemins SD verifies. Il peut etre\n", "checked SD paths. It may be stored\n"},
    {"range autrement ou ne pas etre utilise.\n", "elsewhere or not be used.\n"},
    {"Ce n'est pas une alerte : rien a faire\n", "This is not an alert; no action\n"},
    {"si tu n'en as pas besoin.\n", "if you do not need it.\n"},
    {"Cette application n'a pas ete trouvee\n", "This app was not found among\n"},
    {"parmi les titres installes sur la SD.\n", "the titles installed on the SD card.\n"},
    {"La version trouvee correspond a la\n", "The version found matches the\n"},
    {"derniere version stable consultee.\n", "latest stable version checked.\n"},
    {"Aucune action n'est necessaire.\n", "No action is needed.\n"},
    {"Pas de souci : Checker n'a pas pu\n", "No worries: Checker could not\n"},
    {"confirmer cette version.\n", "confirm this version.\n"},
    {"Cela peut venir du Wi-Fi de la 3DS,\n", "This may be due to the console's Wi-Fi,\n"},
    {"de GitHub momentanement indisponible,\n", "GitHub being temporarily unavailable,\n"},
    {"d'un lien de telechargement change,\n", "a changed download link,\n"},
    {"d'un fichier/version non reconnu.\n", "or an unrecognized file or version.\n"},
    {"Le Checker est en developpement; certains\n", "Checker is in development; some paths\n"},
    {"chemins peuvent ne pas etre detectes.\n", "may not be detected.\n"},
    {"Rien n'a ete modifie. X pour reessayer.\n", "Nothing was changed. Press X to retry.\n"},
    {"OUTIL EN DEVELOPPEMENT", "IN DEVELOPMENT"},
    {"PRENDRE SOIN DE SA 3DS", "CARE FOR YOUR CONSOLE"},
    {"VERIFIER LES VERSIONS", "CHECK VERSIONS"},
    {"SANS RIEN MODIFIER", "WITHOUT CHANGING ANYTHING"},
    {"A : DEMARRER   B : QUITTER", "A: START   B: QUIT"},
    {"CONTROLE %u/%u", "CHECK %u/%u"},
    {"POURQUOI CE RESULTAT ?", "WHY THIS RESULT?"},
    {"RESULTAT DU CONTROLE", "CHECK RESULT"},
    {"ACTIF", "ACTIVE"},
    {"DETAILS SUR LECRAN INFERIEUR", "DETAILS ON LOWER SCREEN"},
    {"Verifier les outils installes ?\n", "Check installed tools?\n"},
    {"Version en developpement : les controles\n", "Development build: checks may not cover\n"},
    {"peuvent ne pas couvrir tous les cas.\n\n", "every possible setup.\n\n"},
    {"Luma active les fonctions personnalisees.\n", "Luma enables custom features.\n"},
    {"GodMode9 sert aux sauvegardes et a la\n", "GodMode9 can help with backups and\n"},
    {"maintenance; les autres outils courants\n", "maintenance; other common SD tools\n"},
    {"de la SD sont aussi verifies.\n\n", "are checked too.\n\n"},
    {"VERIFICATION EN COURS", "CHECKING"},
    {"Etape %u/%u : %s\n\n", "Step %u/%u: %s\n\n"},
    {"Lecture de la carte SD et recherche\n", "Reading the SD card and checking for\n"},
    {"de la derniere version stable.\n", "the latest stable version.\n"},
    {"Cela prend quelques instants.\n", "This may take a moment.\n"},
    {"Rien n'est installe ni modifie.\n", "Nothing is installed or changed.\n"},
    {"RAPPORT DE LA CARTE SD", "SD CARD REPORT"},
    {"  boot.firm SD : %s\n", "  boot.firm on SD: %s\n"},
    {"present", "present"},
    {"absent", "absent"},
    {"dossier present", "folder present"},
    {"dossier facultatif absent", "optional folder absent"},
    {"HAUT/BAS : CHOISIR   A : POURQUOI\n", "UP/DOWN: SELECT   A: WHY\n"},
    {"X : REVERIFIER   START : QUITTER\n", "X: CHECK AGAIN   START: QUIT\n"},
    {"Lecture seule, en developpement.\n", "Read-only, in development.\n"},
    {"Etat : ", "Status: "},
    {"Emplacement : %.36s\n", "Location: %.36s\n"},
    {"boot.firm sur la SD : %s\n", "boot.firm on SD: %s\n"},
    {"Luma peut aussi demarrer depuis CTRNAND.\n", "Luma can also boot from CTRNAND.\n"},
    {"Dossier gm9 (facultatif) : %s\n", "gm9 folder (optional): %s\n"},
    {"Version active", "Active version"},
    {"Version sur SD", "Version on SD"},
    {"%s : %s\nDerniere version : %s\n\n", "%s: %s\nLatest version: %s\n\n"},
    {"\nB : RETOUR   HAUT/BAS : COMPOSANT\n", "\nB: BACK   UP/DOWN: ITEM\n"},
    {"X : REVERIFIER TOUT   START : QUITTER\n", "X: CHECK ALL AGAIN   START: QUIT\n"},
    {"Projet en developpement : resultats indicatifs.\n", "Development build: results are indicative.\n"},
    {"LANGUE PROPOSEE : FRANCAIS", "DETECTED LANGUAGE: ENGLISH"},
    {"FRANCAIS ET ENGLISH UNIQUEMENT", "FRENCH AND ENGLISH ONLY"},
    {"MON ANGLAIS N'EST PAS PARFAIT", "MY ENGLISH MAY CONTAIN ERRORS"},
    {"ERREURS DE TRADUCTION POSSIBLES", "TRANSLATION ERRORS MAY REMAIN"},
    {"A : OUI, GARDER FRANCAIS", "A: YES, KEEP ENGLISH"},
    {"B : NON, PASSER EN ENGLISH", "B: NO, SWITCH TO FRENCH"},
    {"START : QUITTER", "START: QUIT"}
};

static const char *ui_text(const char *text) {
    if (!text || ui_french) return text;
    for (size_t i = 0; i < sizeof(ui_translations) / sizeof(ui_translations[0]); ++i)
        if (strcmp(text, ui_translations[i].french) == 0)
            return ui_translations[i].english;
    return text;
}

static void load_ui_language(void) {
    FILE *file = fopen(LANGUAGE_CONFIG_PATH, "rb");
    if (file) {
        int saved = fgetc(file);
        fclose(file);
        if (saved == 'F' || saved == 'E') {
            ui_french = saved == 'F';
            return;
        }
    }

    // Read only the language enum; do not inspect the console model or region.
    ui_french = false;
    if (R_SUCCEEDED(cfguInit())) {
        u8 system_language = CFG_LANGUAGE_EN;
        if (R_SUCCEEDED(CFGU_GetSystemLanguage(&system_language)))
            ui_french = system_language == CFG_LANGUAGE_FR;
        cfguExit();
    }
    language_confirmation_needed = true;
}

static void save_ui_language(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ShieldChecker", 0777);
    FILE *file = fopen(LANGUAGE_CONFIG_PATH, "wb");
    if (!file) return;
    fputc(ui_french ? 'F' : 'E', file);
    fclose(file);
}

static void reset_check_entries(void) {
    memset(checks, 0, sizeof(checks));
    for (size_t i = 0; i < CHECK_COUNT; ++i) {
        checks[i].name = ui_text(check_definitions[i].name);
        checks[i].description = ui_text(check_definitions[i].description);
        checks[i].api_url = check_definitions[i].api_url;
        checks[i].sd_path = check_definitions[i].sd_path;
        checks[i].kind = check_definitions[i].kind;
        checks[i].title_id = check_definitions[i].title_id;
        checks[i].status = STATUS_UNKNOWN;
        snprintf(checks[i].local_text, sizeof(checks[i].local_text), "--");
        snprintf(checks[i].latest_text, sizeof(checks[i].latest_text), "--");
        if (checks[i].sd_path)
            snprintf(checks[i].detected_path, sizeof(checks[i].detected_path), "%s", checks[i].sd_path);
    }
}

static bool version_parse(const char *text, Version *out) {
    if (!text || !out) return false;
    while (*text && !isdigit((unsigned char)*text) && *text != 'v' && *text != 'V') ++text;
    if (*text == 'v' || *text == 'V') ++text;

    Version parsed = {{0, 0, 0}, 0, false};
    for (unsigned part = 0; part < 3; ++part) {
        if (!isdigit((unsigned char)*text)) return false;
        unsigned value = 0;
        unsigned digits = 0;
        while (isdigit((unsigned char)*text)) {
            unsigned digit = (unsigned)(*text++ - '0');
            if (value > 9999 || (value == 9999 && digit > 9)) return false;
            value = value * 10 + digit;
            ++digits;
            if (digits > 4) return false;
        }
        parsed.part[part] = value;
        ++parsed.count;
        if (part == 0 || part == 1) {
            if (*text != '.') {
                if (part == 1) break;
                return false;
            }
            ++text;
        } else if (*text && *text != '-' && *text != '+' && *text != ' ' &&
                   *text != '\r' && *text != '\n' && *text != '\t') {
            return false;
        }
    }
    if (parsed.count < 2) return false;
    parsed.valid = true;
    *out = parsed;
    return true;
}

static void version_format(const Version *version, char *out, size_t capacity) {
    if (!version || !version->valid || !capacity) {
        if (capacity) snprintf(out, capacity, "--");
        return;
    }
    if (version->count > 2 && version->part[2])
        snprintf(out, capacity, "%u.%u.%u", version->part[0], version->part[1], version->part[2]);
    else
        snprintf(out, capacity, "%u.%u", version->part[0], version->part[1]);
}

static int version_compare(const Version *left, const Version *right) {
    if (!left || !right || !left->valid || !right->valid) return 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (left->part[i] < right->part[i]) return -1;
        if (left->part[i] > right->part[i]) return 1;
    }
    return 0;
}

static bool load_file_bytes(const char *path, u8 **bytes, size_t *length) {
    if (!bytes || !length) return false;
    *bytes = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size <= 0 || (unsigned long)size > MAX_FIRM_SIZE || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    u8 *data = malloc((size_t)size);
    if (!data) { fclose(file); return false; }
    size_t got = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) { free(data); return false; }
    *bytes = data;
    *length = got;
    return true;
}

static bool firm_header_valid(const u8 *data, size_t length) {
    return data && length >= 0x100 && memcmp(data, "FIRM", 4) == 0;
}

static bool read_luma_version(Version *version) {
    s64 packed_info = 0;
    if (R_FAILED(svcGetSystemInfo(&packed_info, 0x10000, 0))) return false;
    u32 packed = (u32)packed_info;
    Version found = {{(packed >> 24) & 0xFF, (packed >> 16) & 0xFF,
                      (packed >> 8) & 0xFF}, 3, true};
    if (found.part[0] == 0 || found.part[0] > 99 || found.part[1] > 99 || found.part[2] > 99)
        return false;
    *version = found;
    return true;
}

static bool parse_embedded_version(const u8 *data, size_t length, const char *marker,
                                   Version *version, char *display, size_t display_capacity) {
    size_t marker_length = strlen(marker);
    bool found = false;
    Version unique = {{0, 0, 0}, 0, false};
    char unique_text[32] = {0};
    for (size_t i = 0; i + marker_length < length; ++i) {
        if (memcmp(data + i, marker, marker_length) != 0) continue;
        size_t start = i + marker_length;
        char candidate[32];
        size_t count = 0;
        while (start + count < length && count + 1 < sizeof(candidate)) {
            u8 ch = data[start + count];
            if (!(isalnum(ch) || ch == '.' || ch == '-' || ch == '+')) break;
            candidate[count++] = (char)ch;
        }
        candidate[count] = '\0';
        Version parsed;
        if (!version_parse(candidate, &parsed)) continue;
        char parsed_text[32];
        version_format(&parsed, parsed_text, sizeof(parsed_text));
        if (found && version_compare(&unique, &parsed) != 0) return false;
        unique = parsed;
        snprintf(unique_text, sizeof(unique_text), "%s", parsed_text);
        found = true;
        i = start + count;
    }
    if (!found) return false;
    *version = unique;
    snprintf(display, display_capacity, "%s", unique_text);
    return true;
}

static bool read_godmode_version(const char *path, Version *version,
                                 char *display, size_t display_capacity) {
    u8 *data = NULL;
    size_t length = 0;
    if (!load_file_bytes(path, &data, &length)) return false;
    bool valid = firm_header_valid(data, length) &&
        parse_embedded_version(data, length, "GodMode9 ", version, display, display_capacity);
    free(data);
    return valid;
}

static PathPresence file_presence(const char *path) {
    struct stat info;
    if (stat(path, &info) == 0) return S_ISREG(info.st_mode) ? PATH_PRESENT : PATH_ERROR;
    if (errno == ENOENT || errno == ENOTDIR) return PATH_ABSENT;
    return PATH_ERROR;
}

static bool load_sd_title_list(void) {
    if (sd_title_list_loaded) return sd_title_list_available;
    sd_title_list_loaded = true;
    if (!service_am) return false;

    u32 count = 0;
    if (R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count))) return false;
    if (count > MAX_SD_TITLE_COUNT) return false;
    if (count == 0) {
        sd_title_count = 0;
        sd_title_list_available = true;
        return true;
    }
    if ((size_t)count > SIZE_MAX / sizeof(*sd_title_ids)) return false;

    u64 *ids = malloc((size_t)count * sizeof(*ids));
    if (!ids) return false;
    u32 titles_read = 0;
    Result result = AM_GetTitleList(&titles_read, MEDIATYPE_SD, count, ids);
    if (R_FAILED(result) || titles_read > count) {
        free(ids);
        return false;
    }
    sd_title_ids = ids;
    sd_title_count = titles_read;
    sd_title_list_available = true;
    return true;
}

static TitleLookup lookup_sd_title(const CheckEntry *entry, AM_TitleInfo *info) {
    if (!entry || !info || !load_sd_title_list()) return TITLE_LOOKUP_UNAVAILABLE;
    bool found = false;
    for (u32 i = 0; i < sd_title_count; ++i) {
        if (sd_title_ids[i] == entry->title_id) {
            found = true;
            break;
        }
    }
    if (!found) return TITLE_LOOKUP_ABSENT;

    u64 title_id = entry->title_id;
    if (R_FAILED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &title_id, info)))
        return TITLE_LOOKUP_UNAVAILABLE;
    return TITLE_LOOKUP_PRESENT;
}

static PathPresence directory_presence(const char *path) {
    struct stat info;
    if (stat(path, &info) == 0) return S_ISDIR(info.st_mode) ? PATH_PRESENT : PATH_ERROR;
    if (errno == ENOENT || errno == ENOTDIR) return PATH_ABSENT;
    return PATH_ERROR;
}

static CheckStatus status_from_versions(const Version *local, const Version *latest) {
    if (!local || !latest || !local->valid || !latest->valid) return STATUS_UNKNOWN;
    int comparison = version_compare(local, latest);
    if (comparison == 0) return STATUS_CURRENT;
    if (comparison < 0) return STATUS_UPDATE;
    // A local build newer than the latest stable release may be a custom or
    // development build. Do not mislabel it as the official current version.
    return STATUS_UNKNOWN;
}

static bool version_to_title_version(const Version *version, u16 *out) {
    if (!version || !out || !version->valid || version->count < 2 ||
        version->part[0] > 63 || version->part[1] > 63 || version->part[2] > 15)
        return false;
    *out = (u16)((version->part[0] << 10) | (version->part[1] << 4) | version->part[2]);
    return *out != 0;
}

static bool title_version_to_version(u16 packed, Version *out) {
    if (!out || packed == 0) return false;
    Version version = {{(packed >> 10) & 0x3F, (packed >> 4) & 0x3F, packed & 0x0F}, 3, true};
    *out = version;
    return true;
}

static bool load_ca_bundle(void) {
    FILE *file = fopen(CA_PATH, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size <= 0 || size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    ca_bundle = malloc((size_t)size);
    if (!ca_bundle) { fclose(file); return false; }
    ca_bundle_size = fread(ca_bundle, 1, (size_t)size, file);
    fclose(file);
    if (ca_bundle_size != (size_t)size) {
        free(ca_bundle);
        ca_bundle = NULL;
        ca_bundle_size = 0;
        return false;
    }
    return true;
}

static void draw_top(void);
static void draw_bottom(void);
static void refresh_ui(void);

static int curl_progress(void *unused, curl_off_t total_down, curl_off_t down,
                         curl_off_t total_up, curl_off_t up) {
    (void)unused; (void)total_down; (void)down; (void)total_up; (void)up;
    hidScanInput();
    if (hidKeysDown() & KEY_B) {
        scan_cancelled = true;
        return 1;
    }
    u64 now = osGetTime();
    if (now - last_animation >= 350) {
        last_animation = now;
        ++animation_frame;
        // gfxSwapBuffers() swaps both screens. Redraw the console screen too,
        // otherwise the lower framebuffer can alternate with an old/black one.
        draw_bottom();
        draw_top();
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return 0;
}

static size_t receive_json(char *data, size_t size, size_t count, void *userdata) {
    HttpBuffer *sink = userdata;
    if (!sink || (size && count > SIZE_MAX / size)) return 0;
    size_t bytes = size * count;
    if (sink->used > sink->capacity || bytes > sink->capacity - sink->used) {
        sink->failed = true;
        return 0;
    }
    memcpy(sink->data + sink->used, data, bytes);
    sink->used += bytes;
    return bytes;
}

static bool init_network(void) {
    if (service_soc) return curl_ready;
    socket_buffer = memalign(0x1000, SOCKET_BUFFER_SIZE);
    if (!socket_buffer) return false;
    Result result = socInit(socket_buffer, SOCKET_BUFFER_SIZE);
    if (R_FAILED(result)) {
        free(socket_buffer);
        socket_buffer = NULL;
        return false;
    }
    service_soc = true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        socExit();
        service_soc = false;
        free(socket_buffer);
        socket_buffer = NULL;
        return false;
    }
    curl_ready = true;
    return true;
}

static void run_network_probe(void) {
    scan_cancelled = false;
    last_network_curl_code = -1;
    last_network_http_status = 0;
    screen_mode = SCREEN_NETWORK_TEST;
    refresh_ui();
    if (!service_romfs || !ca_bundle || !init_network()) {
        diagnostics_set_network(&diagnostic_report, "--", DIAG_UNAVAILABLE,
            "Service reseau ou certificats indisponibles.",
            "Network service or certificates unavailable.");
    } else {
        CURL *curl = curl_easy_init();
        CURLcode result = CURLE_FAILED_INIT;
        long http_status = 0;
        if (curl) {
            struct curl_blob ca = { .data = ca_bundle, .len = ca_bundle_size,
                                    .flags = CURL_BLOB_NOCOPY };
            CURLcode options = CURLE_OK;
#define PROBE_OPT(option, value) do { if (options == CURLE_OK) options = curl_easy_setopt(curl, option, value); } while (0)
            PROBE_OPT(CURLOPT_URL, "https://api.github.com/");
            PROBE_OPT(CURLOPT_USERAGENT, "ShieldChecker/1.0 (read-only diagnostics)");
            PROBE_OPT(CURLOPT_NOBODY, 1L);
            PROBE_OPT(CURLOPT_FOLLOWLOCATION, 0L);
            PROBE_OPT(CURLOPT_PROTOCOLS_STR, "https");
            PROBE_OPT(CURLOPT_SSL_VERIFYPEER, 1L);
            PROBE_OPT(CURLOPT_SSL_VERIFYHOST, 2L);
            PROBE_OPT(CURLOPT_CAINFO_BLOB, &ca);
            PROBE_OPT(CURLOPT_CONNECTTIMEOUT, 8L);
            PROBE_OPT(CURLOPT_TIMEOUT, 15L);
            PROBE_OPT(CURLOPT_NOSIGNAL, 1L);
            PROBE_OPT(CURLOPT_NOPROGRESS, 0L);
            PROBE_OPT(CURLOPT_XFERINFOFUNCTION, curl_progress);
#undef PROBE_OPT
            if (options == CURLE_OK) {
                result = curl_easy_perform(curl);
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
            } else result = options;
            curl_easy_cleanup(curl);
        }
        last_network_curl_code = (int)result;
        last_network_http_status = http_status;
        char value[48];
        const char *fr = "Connexion officielle effectuee.";
        const char *en = "Official connection succeeded.";
        DiagnosticState state = DIAG_OK;
        if (scan_cancelled || result == CURLE_ABORTED_BY_CALLBACK) {
            snprintf(value, sizeof(value), "Annule / Cancelled");
            fr = "Test annule par l'utilisateur.";
            en = "Test cancelled by the user.";
            state = DIAG_UNAVAILABLE;
        } else if (result == CURLE_COULDNT_RESOLVE_HOST) {
            snprintf(value, sizeof(value), "DNS");
            fr = "Le nom du serveur n'a pas pu etre trouve. La console n'est pas forcement en panne.";
            en = "The server name could not be found. This does not prove a console fault.";
            state = DIAG_UNAVAILABLE;
        } else if (result == CURLE_OPERATION_TIMEDOUT) {
            snprintf(value, sizeof(value), "Delai / Timeout");
            fr = "Le serveur a mis trop de temps a repondre. Tu peux reessayer plus tard.";
            en = "The server took too long to respond. You can try again later.";
            state = DIAG_UNAVAILABLE;
        } else if (result == CURLE_PEER_FAILED_VERIFICATION ||
                   result == CURLE_SSL_CONNECT_ERROR) {
            snprintf(value, sizeof(value), "HTTPS");
            fr = "La connexion securisee n'a pas pu etre etablie. Aucun acces non securise n'a ete tente.";
            en = "A secure connection could not be made. No insecure connection was attempted.";
            state = DIAG_UNAVAILABLE;
        } else if (result != CURLE_OK || http_status != 200) {
            snprintf(value, sizeof(value), "Serveur / Server");
            fr = "Le serveur n'a pas repondu comme prevu. Cela ne prouve pas un probleme de la 3DS.";
            en = "The server replied unexpectedly. This does not prove a 3DS problem.";
            state = DIAG_UNAVAILABLE;
        } else {
            snprintf(value, sizeof(value), "Accessible");
        }
        diagnostics_set_network(&diagnostic_report, value, state, fr, en);
    }
    selected_diagnostic = diagnostic_report.count - 1;
    screen_mode = SCREEN_DIAGNOSTICS;
    refresh_ui();
}

static bool http_get_payload(size_t index) {
    if (!service_soc || !curl_ready || !ca_bundle || index >= CHECK_COUNT || !checks[index].api_url)
        return false;
    memset(api_buffer, 0, sizeof(api_buffer));
    HttpBuffer sink = {api_buffer, 0, sizeof(api_buffer) - 1, false};
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_blob ca = { .data = ca_bundle, .len = ca_bundle_size, .flags = CURL_BLOB_NOCOPY };
    CURLcode option_result = CURLE_OK;
#define SETOPT(opt, value) do { if (option_result == CURLE_OK) option_result = curl_easy_setopt(curl, opt, value); } while (0)
    SETOPT(CURLOPT_URL, checks[index].api_url);
    SETOPT(CURLOPT_USERAGENT, "ShieldChecker/1.0 (read-only status check)");
    SETOPT(CURLOPT_HTTPHEADER, NULL);
    SETOPT(CURLOPT_FOLLOWLOCATION, 1L);
    SETOPT(CURLOPT_MAXREDIRS, 4L);
    SETOPT(CURLOPT_PROTOCOLS_STR, "https");
    SETOPT(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
    SETOPT(CURLOPT_CAINFO_BLOB, &ca);
    SETOPT(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SETOPT(CURLOPT_CONNECTTIMEOUT, 8L);
    SETOPT(CURLOPT_TIMEOUT, 18L);
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_NOPROGRESS, 0L);
    SETOPT(CURLOPT_XFERINFOFUNCTION, curl_progress);
    SETOPT(CURLOPT_WRITEFUNCTION, receive_json);
    SETOPT(CURLOPT_WRITEDATA, &sink);
#undef SETOPT
    if (option_result != CURLE_OK) { curl_easy_cleanup(curl); return false; }
    scan_phase = "RECHERCHE DE LA VERSION";
    active_check = index;
    last_animation = 0;
    draw_bottom();
    draw_top();
    gfxFlushBuffers();
    gfxSwapBuffers();
    CURLcode result = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && http_status == 200 && !sink.failed && sink.used > 0;
}

static bool fetch_latest_version(size_t index, Version *version, char *display,
                                 size_t capacity, u16 *title_version) {
    if (index >= CHECK_COUNT || !version || !display || capacity == 0) return false;
    if (!http_get_payload(index)) return false;

    if (checks[index].kind == CHECK_HSHOP_TITLE) {
        Version parsed;
        if (!version_parse(api_buffer, &parsed) || !version_to_title_version(&parsed, title_version))
            return false;
        *version = parsed;
        version_format(&parsed, display, capacity);
        checks[index].latest_title_version = *title_version;
        checks[index].latest_title_version_valid = true;
        return true;
    }

    json_error_t error;
    json_t *root = json_loadb(api_buffer, strlen(api_buffer), JSON_REJECT_DUPLICATES, &error);
    if (!root) return false;
    json_t *tag = json_object_get(root, "tag_name");
    const char *tag_text = json_is_string(tag) ? json_string_value(tag) : NULL;
    bool parsed = tag_text && version_parse(tag_text, version);
    if (parsed) version_format(version, display, capacity);

    if (parsed && checks[index].kind == CHECK_SEMVER_TITLE) {
        parsed = version_to_title_version(version, title_version);
        if (parsed) {
            checks[index].latest_title_version = *title_version;
            checks[index].latest_title_version_valid = true;
        }
    }
    json_decref(root);
    return parsed;
}

static void inspect_luma(bool online) {
    CheckEntry *entry = &checks[0];
    PathPresence presence = file_presence(entry->sd_path);
    entry->sd_file_checked = presence != PATH_ERROR;
    entry->sd_file_present = presence == PATH_PRESENT;
    entry->checked = true;
    snprintf(entry->detected_path, sizeof(entry->detected_path), "%s",
             entry->sd_file_present ? entry->sd_path :
             presence == PATH_ABSENT ? "boot.firm absent de la SD" :
                                       "Acces au fichier SD non confirme");

    // Read the active Luma version independently of the SD boot file. Luma can
    // also start from CTRNAND, so an absent SD copy must not be reported as a
    // missing installation or a console fault.
    if (!read_luma_version(&entry->local)) {
        entry->status = STATUS_UNKNOWN;
        return;
    }
    entry->installed = true;
    version_format(&entry->local, entry->local_text, sizeof(entry->local_text));
    if (!online || !fetch_latest_version(0, &entry->latest, entry->latest_text,
                              sizeof(entry->latest_text), NULL)) {
        entry->status = STATUS_UNKNOWN;
        return;
    }
    entry->status = status_from_versions(&entry->local, &entry->latest);
}

static void inspect_godmode9(bool online) {
    static const char *paths[] = {
        "sdmc:/luma/payloads/GodMode9.firm",
        "sdmc:/gm9/payloads/GodMode9.firm",
        "sdmc:/GodMode9.firm",
        "sdmc:/gm9/GodMode9.firm"
    };
    CheckEntry *entry = &checks[1];
    entry->installed = false;
    entry->detected_path[0] = '\0';
    entry->checked = true;
    PathPresence gm9_dir = directory_presence("sdmc:/gm9");
    entry->support_dir_checked = gm9_dir != PATH_ERROR;
    entry->support_dir_present = gm9_dir == PATH_PRESENT;
    bool have_version = false;
    Version first = {{0, 0, 0}, 0, false};
    char first_display[32] = {0};
    bool ambiguous = false;
    bool uncertain_path = false;

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        PathPresence presence = file_presence(paths[i]);
        if (presence == PATH_ABSENT) continue;
        if (presence == PATH_ERROR) { uncertain_path = true; continue; }
        entry->installed = true;
        if (!entry->detected_path[0]) snprintf(entry->detected_path, sizeof(entry->detected_path), "%s", paths[i]);
        Version found;
        char display[32] = {0};
        if (!read_godmode_version(paths[i], &found, display, sizeof(display))) {
            ambiguous = true;
            continue;
        }
        if (have_version && version_compare(&first, &found) != 0) ambiguous = true;
        if (!have_version) {
            first = found;
            snprintf(first_display, sizeof(first_display), "%s", display);
            have_version = true;
        }
    }

    if (!entry->installed) {
        entry->status = uncertain_path ? STATUS_UNKNOWN : STATUS_MISSING;
        snprintf(entry->detected_path, sizeof(entry->detected_path),
                 uncertain_path ? "Acces SD non confirme" : "Payload absent des chemins SD verifies");
        return;
    }
    if (ambiguous || !have_version) {
        entry->status = STATUS_UNKNOWN;
        return;
    }
    entry->local = first;
    snprintf(entry->local_text, sizeof(entry->local_text), "%s", first_display);
    if (!online || !fetch_latest_version(1, &entry->latest, entry->latest_text,
                              sizeof(entry->latest_text), NULL)) {
        entry->status = STATUS_UNKNOWN;
        return;
    }
    entry->status = status_from_versions(&entry->local, &entry->latest);
}

static void inspect_title_app(size_t index, bool online) {
    CheckEntry *entry = &checks[index];
    entry->checked = true;
    AM_TitleInfo info;
    TitleLookup lookup = lookup_sd_title(entry, &info);
    if (lookup == TITLE_LOOKUP_ABSENT) {
        entry->status = STATUS_MISSING;
        snprintf(entry->detected_path, sizeof(entry->detected_path), "Titre non trouve sur la SD");
        return;
    }
    if (lookup != TITLE_LOOKUP_PRESENT) {
        entry->status = STATUS_UNKNOWN;
        snprintf(entry->detected_path, sizeof(entry->detected_path), "Lecture des titres SD indisponible");
        return;
    }

    entry->installed = true;
    entry->local_title_version = info.version;
    snprintf(entry->detected_path, sizeof(entry->detected_path), "Titre installe sur la SD");
    if (entry->kind == CHECK_CIA_VERSION_TITLE) {
        // AM exposes the CIA revision, not the public release tag. These
        // numbers have different meanings and must never be compared.
        snprintf(entry->local_text, sizeof(entry->local_text), "--");
        if (online) fetch_latest_version(index, &entry->latest, entry->latest_text,
                                         sizeof(entry->latest_text), NULL);
        entry->status = STATUS_UNKNOWN;
        return;
    } else {
        if (!title_version_to_version(info.version, &entry->local)) {
            entry->status = STATUS_UNKNOWN;
            return;
        }
        version_format(&entry->local, entry->local_text, sizeof(entry->local_text));
    }

    if (!online || !fetch_latest_version(index, &entry->latest, entry->latest_text,
                              sizeof(entry->latest_text), &entry->latest_title_version)) {
        entry->status = STATUS_UNKNOWN;
        return;
    }
    if (!entry->latest_title_version_valid) {
        entry->status = STATUS_UNKNOWN;
        return;
    }

    if (entry->local_title_version == entry->latest_title_version)
        entry->status = STATUS_CURRENT;
    else if (entry->local_title_version < entry->latest_title_version)
        entry->status = STATUS_UPDATE;
    else
        entry->status = STATUS_UNKNOWN;
}

static const char *status_label(const CheckEntry *entry) {
    if (!entry) return ui_text("STATUT INDISPONIBLE");
    if (entry->kind == CHECK_CIA_VERSION_TITLE && entry->installed)
        return ui_french ? "VERSION LOCALE NON LISIBLE" : "LOCAL VERSION UNKNOWN";
    switch (entry->status) {
        case STATUS_MISSING:
            return ui_french ? "NON DETECTE AUX EMPLACEMENTS TESTES" :
                               "NOT FOUND IN CHECKED LOCATIONS";
        case STATUS_CURRENT: return ui_text("VERIFIE ET A JOUR");
        case STATUS_UPDATE: return ui_text("VERSION PLUS RECENTE");
        default: return ui_french ? "NON VERIFIE (PAS UNE PANNE)" :
                                   "NOT VERIFIED (NOT A FAULT)";
    }
}

static void print_status_explanation(const CheckEntry *entry) {
    if (!entry) return;
    if (entry->kind == CHECK_CIA_VERSION_TITLE && entry->installed) {
        if (ui_french) {
            printf("Shield Updater est installe sur SD.\n");
            printf("Checker ne peut pas lire sa version\n");
            printf("publique installee. Il ne conclut\n");
            printf("donc pas sur une mise a jour.\n\n");
            if (entry->latest.valid)
                printf("Version publiee vue : %s\n", entry->latest_text);
            else
                printf("Version publiee non verifiee.\n");
            printf("Tu peux ouvrir Shield Updater pour\n");
            printf("verifier ses mises a jour. Rien ici\n");
            printf("ne signale une panne.\n");
        } else {
            printf("Shield Updater is installed on SD.\n");
            printf("Checker cannot read its installed\n");
            printf("public version, so it cannot say\n");
            printf("whether an update is needed.\n\n");
            if (entry->latest.valid)
                printf("Published version seen: %s\n", entry->latest_text);
            else
                printf("Published version not checked.\n");
            printf("You can open Shield Updater to\n");
            printf("check updates. Nothing here shows\n");
            printf("a console fault.\n");
        }
        return;
    }
    if (entry->kind == CHECK_HSHOP_TITLE) {
        if (entry->status == STATUS_MISSING)
            printf("%s", ui_text("Client non trouve sur la SD.\n"));
        else if (entry->status == STATUS_CURRENT)
            printf("%s", ui_text("La version installee correspond a la\nversion publiee consultee.\n"));
        else if (entry->status == STATUS_UPDATE)
            printf("%s", ui_text("Une version publiee est plus recente.\n"));
        else if (!online_requested)
            printf("%s\n", ui_french ? "Comparaison en ligne non lancee." :
                                  "Online comparison not started.");
        else
            printf("%s\n", ui_french ? "Version non verifiee cette fois." :
                                  "Version not verified this time.");
        return;
    }
    if (entry->status == STATUS_UPDATE && entry->kind == CHECK_LUMA) {
        printf("%s", ui_text("Une version plus recente existe. Cela\n"));
        printf("%s", ui_text("ne signifie pas qu'il y a un probleme.\n"));
        printf("%s", ui_text("Luma permet les fonctions personnalisees\n"));
        printf("%s", ui_text("de la console. Consulte les notes\n"));
        printf("%s", ui_text("officielles pour voir les changements.\n"));
        printf("%s", ui_text("Sauvegarde la SD avant toute action.\n"));
        printf("%s", ui_text("Checker ne met rien a jour.\n"));
    } else if (entry->status == STATUS_UPDATE) {
        printf("%s", ui_text("Une version plus recente est disponible.\n"));
        printf("%s\n", ui_french ? "Ta version n'est pas declaree en panne." :
                              "Your version is not marked faulty.");
        if (entry->kind == CHECK_GODMODE9) {
            printf("%s", ui_text("GodMode9 aide aux sauvegardes et a\n"));
            printf("%s", ui_text("certaines taches de maintenance.\n"));
            printf("%s", ui_text("Il est facultatif selon tes besoins.\n"));
        } else if (entry->kind == CHECK_SEMVER_TITLE) {
            printf("%s", ui_text("Universal Updater sert a parcourir\n"));
            printf("%s", ui_text("et installer des homebrews.\n"));
        } else {
            printf("%s", ui_text("Shield Updater gere les applications\n"));
            printf("%s", ui_text("Shield disponibles.\n"));
        }
        printf("%s", ui_text("Checker ne telecharge ni n'installe.\n"));
    } else if (entry->status == STATUS_MISSING) {
        if (entry->kind == CHECK_GODMODE9) {
            printf("%s", ui_text("GodMode9.firm n'a pas ete trouve dans\n"));
            printf("%s", ui_text("les chemins SD verifies. Il peut etre\n"));
            printf("%s", ui_text("range autrement ou ne pas etre utilise.\n"));
            printf("%s", ui_text("Ce n'est pas une alerte : rien a faire\n"));
            printf("%s", ui_text("si tu n'en as pas besoin.\n"));
        } else {
            printf("%s", ui_text("Cette application n'a pas ete trouvee\n"));
            printf("%s", ui_text("parmi les titres installes sur la SD.\n"));
        }
    } else if (entry->status == STATUS_CURRENT) {
        printf("%s", ui_text("La version trouvee correspond a la\n"));
        printf("%s", ui_text("derniere version stable consultee.\n"));
        printf("%s", ui_text("Aucune action n'est necessaire.\n"));
    } else {
        printf("%s\n", ui_french ? "Cette version n'a pas pu etre" :
                              "This version could not be");
        printf("%s\n", ui_french ? "verifiee cette fois." :
                              "verified this time.");
        if (!online_requested) {
            printf("%s\n", ui_french ? "Le controle en ligne n'a pas ete" :
                                  "The online check has not been");
            printf("%s\n", ui_french ? "lance. X permet de l'essayer." :
                                  "started. Press X to try it.");
            printf("%s\n", ui_french ? "Rien n'est casse ou modifie." :
                                  "Nothing is broken or changed.");
            return;
        }
        printf("%s\n", ui_french ? "Le reseau, le serveur ou le format" :
                              "The network, server or version");
        printf("%s\n", ui_french ? "de version peut en etre la cause." :
                              "format could be the reason.");
        printf("%s\n", ui_french ? "La console reste utilisable." :
                              "Your console remains usable.");
        printf("%s\n", ui_french ? "X permet de reessayer plus tard." :
                              "Press X to retry later.");
    }
}

static void run_checks(bool online) {
    scan_cancelled = false;
    reset_check_entries();
    screen_mode = SCREEN_SCANNING;
    for (size_t i = 0; i < CHECK_COUNT; ++i) {
        active_check = i;
        scan_step = (unsigned)i + 1;
        scan_phase = "LECTURE DE LA SD";
        refresh_ui();
        bool can_query = online && !scan_cancelled;
        if (checks[i].kind == CHECK_LUMA) inspect_luma(can_query);
        else if (checks[i].kind == CHECK_GODMODE9) inspect_godmode9(can_query);
        else inspect_title_app(i, can_query);
    }
    selected_check = 0;
    have_results = true;
    last_result_screen = SCREEN_RESULTS;
    screen_mode = SCREEN_RESULTS;
    refresh_ui();
}

static void pixel(u8 *frame, int x, int y, u8 r, u8 g, u8 b) {
    if (!frame || x < 0 || y < 0 || x >= 400 || y >= 240) return;
    size_t at = ((size_t)x * 240 + (239 - y)) * 3;
    frame[at] = b;
    frame[at + 1] = g;
    frame[at + 2] = r;
}

static void fill_rect(u8 *frame, int x, int y, int width, int height, u8 r, u8 g, u8 b) {
    for (int yy = y; yy < y + height; ++yy)
        for (int xx = x; xx < x + width; ++xx)
            pixel(frame, xx, yy, r, g, b);
}

static void draw_line(u8 *frame, int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        pixel(frame, x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

static void draw_circle(u8 *frame, int cx, int cy, int radius, u8 r, u8 g, u8 b) {
    int x = radius, y = 0, error = 1 - radius;
    while (x >= y) {
        pixel(frame, cx + x, cy + y, r, g, b); pixel(frame, cx + y, cy + x, r, g, b);
        pixel(frame, cx - y, cy + x, r, g, b); pixel(frame, cx - x, cy + y, r, g, b);
        pixel(frame, cx - x, cy - y, r, g, b); pixel(frame, cx - y, cy - x, r, g, b);
        pixel(frame, cx + y, cy - x, r, g, b); pixel(frame, cx + x, cy - y, r, g, b);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x) + 1; }
    }
}

static void top_text(u8 *frame, int x, int y, const char *text, u8 r, u8 g, u8 b) {
    if (!font_atlas || !text) return;
    // Keep a small margin from the right frame. The font is 10 px wide, so
    // clipping at the pixel boundary alone leaves half-glyphs against the UI.
    int max_letters = (376 - x) / 10;
    if (max_letters <= 0) return;
    size_t text_length = strlen(text);
    bool clipped = text_length > (size_t)max_letters;
    int letters = clipped && max_letters > 3 ? max_letters - 3 : max_letters;
    for (int letter = 0; letter < letters && text[letter]; ++letter) {
        unsigned char ch = (unsigned char)text[letter];
        if (ch < 32 || ch > 126) ch = '?';
        const u8 *glyph = font_atlas + (ch - 32) * 160;
        for (int row = 0; row < 16; ++row)
            for (int col = 0; col < 10; ++col)
                if (glyph[row * 10 + col] > 96)
                    pixel(frame, x + letter * 10 + col, y + row, r, g, b);
    }
    if (clipped && max_letters > 3) {
        for (int dot = 0; dot < 3; ++dot) {
            const u8 *glyph = font_atlas + ('.' - 32) * 160;
            int letter = letters + dot;
            for (int row = 0; row < 16; ++row)
                for (int col = 0; col < 10; ++col)
                    if (glyph[row * 10 + col] > 96)
                        pixel(frame, x + letter * 10 + col, y + row, r, g, b);
        }
    }
}

static void draw_brand_mark(u8 *frame) {
    draw_circle(frame, 36, 34, 17, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    draw_circle(frame, 36, 34, 13, COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    draw_line(frame, 48, 46, 60, 58, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    draw_line(frame, 30, 34, 35, 39, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
    draw_line(frame, 35, 39, 44, 28, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
}

static u8 *top_frame(void) {
    return gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
}

static void draw_top(void) {
    u8 *frame = top_frame();
    if (!frame) return;
    if (screen_mode == SCREEN_DISPLAY_TEST && !display_lower) {
        static const u8 colors[5][3] = {
            {0, 0, 0}, {255, 255, 255}, {255, 0, 0},
            {0, 255, 0}, {0, 0, 255}
        };
        fill_rect(frame, 0, 0, 400, 240,
                  colors[display_color][0], colors[display_color][1],
                  colors[display_color][2]);
        return;
    }
    for (int y = 0; y < 240; ++y) {
        u8 r = (u8)(COLOR_BG_R + y / 55);
        u8 g = (u8)(COLOR_BG_G + y / 26);
        u8 b = (u8)(COLOR_BG_B + y / 18);
        fill_rect(frame, 0, y, 400, 1, r, g, b);
    }
    for (int x = 7; x < 393; ++x) {
        pixel(frame, x, 7, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        pixel(frame, x, 232, 23, 82, 118);
    }
    for (int y = 7; y < 233; ++y) {
        pixel(frame, 7, y, 23, 82, 118);
        pixel(frame, 392, y, 23, 82, 118);
    }
    fill_rect(frame, 12, 12, 376, 49, COLOR_PANEL_R, COLOR_PANEL_G, COLOR_PANEL_B);
    draw_brand_mark(frame);
    top_text(frame, 69, 15, "SHIELD", COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
    top_text(frame, 139, 15, "CHECKER", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    top_text(frame, 69, 36, ui_french ? "COMPRENDRE SA CONSOLE" :
             "UNDERSTAND YOUR CONSOLE", COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    fill_rect(frame, 12, 64, 376, 2, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);

    if (screen_mode == SCREEN_LANGUAGE) {
        top_text(frame, 47, 84, ui_text("LANGUE PROPOSEE : FRANCAIS"),
                 COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 51, 111, ui_text("FRANCAIS ET ENGLISH UNIQUEMENT"),
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 62, 138, ui_text("MON ANGLAIS N'EST PAS PARFAIT"),
                 COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
        top_text(frame, 72, 165, ui_text("ERREURS DE TRADUCTION POSSIBLES"),
                 COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    } else if (screen_mode == SCREEN_WELCOME) {
        top_text(frame, 44, 88, ui_text("PRENDRE SOIN DE SA 3DS"), COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 55, 116, ui_french ? "DIAGNOSTICS ET VERSIONS" :
                 "DIAGNOSTICS AND VERSIONS", COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 46, 144, ui_text("SANS RIEN MODIFIER"), COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
        fill_rect(frame, 57, 187, 286, 1, 24, 78, 112);
        top_text(frame, 91, 201, ui_french ? "A : MENU   B : QUITTER" :
                 "A: MENU   B: QUIT", COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
    } else if (screen_mode == SCREEN_MENU) {
        top_text(frame, 24, 78, ui_french ? "CHOISIR UN CONTROLE" : "CHOOSE A CHECK",
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        for (size_t i = 0; i < MENU_COUNT; ++i) {
            const char *name = ui_french ? menu_fr[i] : menu_en[i];
            top_text(frame, 25, 101 + (int)i * 17, name,
                     i == selected_menu ? COLOR_WHITE_R : COLOR_MUTED_R,
                     i == selected_menu ? COLOR_WHITE_G : COLOR_MUTED_G,
                     i == selected_menu ? COLOR_WHITE_B : COLOR_MUTED_B);
        }
    } else if (screen_mode == SCREEN_DIAGNOSTICS || screen_mode == SCREEN_DIAG_DETAILS) {
        if (diagnostic_report.count) {
            DiagnosticItem *item = &diagnostic_report.items[selected_diagnostic];
            top_text(frame, 24, 83,
                     screen_mode == SCREEN_DIAG_DETAILS ?
                     (ui_french ? "DETAIL DU CONTROLE" : "CHECK DETAIL") :
                     (ui_french ? "RESULTATS LOCAUX" : "LOCAL RESULTS"),
                     COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
            top_text(frame, 24, 111, ui_french ? item->label_fr : item->label_en,
                     COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
            top_text(frame, 24, 139, item->value, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
            top_text(frame, 24, 170, diagnostics_state_name(item->state, ui_french),
                     item->state == DIAG_OK ? COLOR_GREEN_R : COLOR_CYAN_R,
                     item->state == DIAG_OK ? COLOR_GREEN_G : COLOR_CYAN_G,
                     item->state == DIAG_OK ? COLOR_GREEN_B : COLOR_CYAN_B);
        }
    } else if (screen_mode == SCREEN_TROUBLE_MENU || screen_mode == SCREEN_TROUBLE_DETAILS) {
        top_text(frame, 24, 83,
                 ui_french ? "AIDE AU DIAGNOSTIC" : "TROUBLESHOOTING",
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 24, 119,
                 ui_french ? symptom_fr[selected_symptom] : symptom_en[selected_symptom],
                 COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 24, 170,
                 ui_french ? "OBSERVATIONS, PAS DE CERTITUDE" : "OBSERVATIONS, NOT CERTAINTY",
                 COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    } else if (screen_mode == SCREEN_INPUT_TEST) {
        top_text(frame, 24, 88,
                 ui_french ? "TEST DES COMMANDES" : "INPUT TEST",
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 24, 127,
                 ui_french ? "PRESSE LES TOUCHES" : "PRESS THE BUTTONS",
                 COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 24, 170,
                 ui_french ? "B : RETOUR" : "B: BACK",
                 COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    } else if (screen_mode == SCREEN_SETTINGS) {
        top_text(frame, 24, 88, ui_french ? "REGLAGES" : "SETTINGS",
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 24, 123,
                 advanced_view ? (ui_french ? "VUE AVANCEE" : "ADVANCED VIEW") :
                 (ui_french ? "VUE SIMPLE" : "SIMPLE VIEW"),
                 COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 24, 166,
                 ui_french ? "LECTURE SEULE" : "READ ONLY",
                 COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    } else if (screen_mode == SCREEN_DISPLAY_TEST) {
        top_text(frame, 24, 88,
                 ui_french ? "TEST VISUEL ECRAN BAS" : "BOTTOM SCREEN TEST",
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 24, 127,
                 ui_french ? "A : COULEUR SUIVANTE" : "A: NEXT COLOR",
                 COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 24, 170,
                 ui_french ? "X : ECRAN HAUT  B : RETOUR" : "X: TOP SCREEN  B: BACK",
                 COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    } else if (screen_mode == SCREEN_NETWORK_TEST) {
        top_text(frame, 24, 88,
                 ui_french ? "TEST RESEAU OPTIONNEL" : "OPTIONAL NETWORK TEST",
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 24, 127, "HTTPS: api.github.com",
                 COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 24, 170,
                 ui_french ? "B : ANNULER" : "B: CANCEL",
                 COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    } else if (screen_mode == SCREEN_SCANNING) {
        char step[32];
        snprintf(step, sizeof(step), ui_text("CONTROLE %u/%u"), scan_step, CHECK_COUNT);
        top_text(frame, 24, 83, step, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 24, 107, checks[active_check].name, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 24, 132, ui_text(scan_phase), COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
        top_text(frame, 24, 155, checks[active_check].local_text, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        for (int i = 0; i < 8; ++i) {
            int idx = (int)((animation_frame / 2 + (unsigned)i) % 8);
            int x = 351 + (int)(18 * cosf((float)idx * 0.785398163f));
            int y = 142 + (int)(18 * sinf((float)idx * 0.785398163f));
            pixel(frame, x, y, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
            pixel(frame, x + 1, y, COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        }
        fill_rect(frame, 24, 194, 352, 8, 25, 57, 83);
        fill_rect(frame, 24, 194, (int)(352 * scan_step / CHECK_COUNT), 8,
                  COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
    } else {
        CheckEntry *entry = &checks[selected_check];
        top_text(frame, 24, 82, ui_text(screen_mode == SCREEN_DETAILS ? "POURQUOI CE RESULTAT ?" : "RESULTAT DU CONTROLE"),
                 COLOR_CYAN_R, COLOR_CYAN_G, COLOR_CYAN_B);
        top_text(frame, 24, 106, entry->name, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        top_text(frame, 24, 130, status_label(entry),
                 entry->status == STATUS_CURRENT ? COLOR_GREEN_R :
                 entry->status == STATUS_UPDATE ? COLOR_CYAN_R : COLOR_MUTED_R,
                 entry->status == STATUS_CURRENT ? COLOR_GREEN_G :
                 entry->status == STATUS_UPDATE ? COLOR_CYAN_G : COLOR_MUTED_G,
                 entry->status == STATUS_CURRENT ? COLOR_GREEN_B :
                 entry->status == STATUS_UPDATE ? COLOR_CYAN_B : COLOR_MUTED_B);
        char version_line[38];
        if (entry->kind == CHECK_CIA_VERSION_TITLE && entry->installed)
            snprintf(version_line, sizeof(version_line), "%s",
                     ui_french ? "VERSION LOCALE INCONNUE" : "LOCAL VERSION UNKNOWN");
        else
            snprintf(version_line, sizeof(version_line), "%s %s / %s",
                     ui_text(entry->kind == CHECK_LUMA ? "ACTIF" : "SD"),
                     entry->local_text, entry->latest_text);
        top_text(frame, 24, 158, version_line, COLOR_WHITE_R, COLOR_WHITE_G, COLOR_WHITE_B);
        if (screen_mode == SCREEN_DETAILS)
            top_text(frame, 24, 184, ui_text("DETAILS SUR LECRAN INFERIEUR"), COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
        else
            top_text(frame, 24, 184, ui_text("OUTIL EN DEVELOPPEMENT"), COLOR_MUTED_R, COLOR_MUTED_G, COLOR_MUTED_B);
    }
}

static void print_status_color(CheckStatus status) {
    if (status == STATUS_CURRENT) printf("\x1b[32m");
    else if (status == STATUS_UPDATE) printf("\x1b[33m");
    else printf("\x1b[37m");
}

static const char *diagnostic_value(const char *label_fr) {
    for (size_t i = 0; i < diagnostic_report.count; ++i)
        if (strcmp(diagnostic_report.items[i].label_fr, label_fr) == 0)
            return diagnostic_report.items[i].value;
    return "--";
}

static void print_troubleshooting(void) {
    switch (selected_symptom) {
        case 0:
            printf("%s: %s\n%s: %s\n\n",
                   ui_french ? "Volume" : "Volume", diagnostic_value("Curseur volume"),
                   ui_french ? "Casque" : "Headset", diagnostic_value("Casque"));
            printf("%s\n", ui_french ? "Verifier volume, casque et app." :
                                     "Check volume, headset and app.");
            printf("%s\n", ui_french ? "Haut-parleur non testable ici." :
                                     "Speaker cannot be tested here.");
            break;
        case 1:
            printf("Wi-Fi: %s\n\n", diagnostic_value("Signal Wi-Fi"));
            printf("%s\n", ui_french ? "Un signal nul ne prouve pas une" :
                                     "Zero bars do not prove a");
            printf("%s\n", ui_french ? "panne materielle. Verifier le" :
                                     "hardware fault. Check the");
            printf("%s\n", ui_french ? "point d'acces et la connexion." :
                                     "access point and connection.");
            break;
        case 2:
            printf("%s\n", ui_french ? "Une autre app peut echouer pour" :
                                     "Another app may fail due to");
            printf("%s\n", ui_french ? "sa SD, son code ou sa version." :
                                     "SD, code or version issues.");
            printf("%s\n", ui_french ? "Checker ne peut pas analyser" :
                                     "Checker cannot inspect");
            printf("%s\n", ui_french ? "un crash externe automatiquement." :
                                     "external crashes automatically.");
            break;
        case 3:
            printf("SD: %s\n\n", diagnostic_value("Carte SD"));
            printf("%s\n", ui_french ? "Verifier la carte et son contact" :
                                     "Check the card and its contact");
            printf("%s\n", ui_french ? "console eteinte. Aucun fichier" :
                                     "with power off. No file");
            printf("%s\n", ui_french ? "n'est modifie par Checker." :
                                     "is changed by Checker.");
            break;
        case 4:
            printf("%s: %s\n\n", ui_french ? "RAM app" : "App RAM",
                   diagnostic_value("RAM region app"));
            printf("%s\n", ui_french ? "Cette valeur ne mesure pas la" :
                                     "This value does not measure");
            printf("%s\n", ui_french ? "charge CPU des autres apps." :
                                     "other apps' CPU usage.");
            break;
        case 5:
            printf("%s\n", ui_french ? "X : ouvrir le test des touches." :
                                     "X: open the button test.");
            printf("%s\n", ui_french ? "Une touche non pressee n'est" :
                                     "An unpressed button is not");
            printf("%s\n", ui_french ? "pas declaree defectueuse." :
                                     "declared faulty.");
            break;
        case 6:
            printf("%s\n", ui_french ? "Y : ouvrir le test des couleurs." :
                                     "Y: open the color test.");
            printf("%s\n", ui_french ? "Observer soi-meme les pixels." :
                                     "Observe the pixels yourself.");
            break;
        case 7:
            printf("%s: %s\n%s: %s\n\n",
                   ui_french ? "Batterie" : "Battery", diagnostic_value("Batterie"),
                   ui_french ? "Charge" : "Charging", diagnostic_value("Charge"));
            printf("%s\n", ui_french ? "Niveau 0-5, pas une mesure" :
                                     "Level 0-5, not a battery");
            printf("%s\n", ui_french ? "d'usure ou de capacite reelle." :
                                     "health or capacity reading.");
            break;
    }
    printf("\n%s\n", ui_french ? "Hypotheses seulement; B : retour" :
                              "Possible causes only; B: back");
}

static void print_wrapped(const char *message, size_t width, size_t max_lines) {
    if (!message || width == 0) return;
    size_t length = strlen(message);
    size_t cursor = 0;
    for (size_t line = 0; line < max_lines && cursor < length; ++line) {
        while (cursor < length && message[cursor] == ' ') ++cursor;
        if (cursor >= length) break;
        size_t end = cursor + width;
        if (end > length) end = length;
        if (end < length && message[end] != ' ') {
            size_t space = end;
            while (space > cursor && message[space] != ' ') --space;
            if (space > cursor) end = space;
        }
        printf("%.*s\n", (int)(end - cursor), message + cursor);
        cursor = end;
    }
}

static void draw_bottom(void) {
    if (screen_mode == SCREEN_DISPLAY_TEST && display_lower) {
        static const u8 colors[5][3] = {
            {0, 0, 0}, {255, 255, 255}, {255, 0, 0},
            {0, 255, 0}, {0, 0, 255}
        };
        u8 *frame = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        if (frame) {
            for (int x = 0; x < 320; ++x)
                for (int y = 0; y < 240; ++y) {
                    size_t at = ((size_t)x * 240 + y) * 3;
                    frame[at] = colors[display_color][2];
                    frame[at + 1] = colors[display_color][1];
                    frame[at + 2] = colors[display_color][0];
                }
        }
        return;
    }
    consoleClear();
    printf("\x1b[36m%s\x1b[0m\n", ui_text("SHIELD CHECKER"));
    printf("----------------------------------------\n");
    if (screen_mode == SCREEN_LANGUAGE) {
        if (ui_french) {
            printf("\nLangue proposee : Francais\n");
            printf("Francais et English uniquement.\n");
            printf("Mon anglais peut contenir des erreurs.\n\n");
            printf("A : oui, garder Francais\n");
            printf("B : non, passer en English\n");
        } else {
            printf("\nSuggested language: English\n");
            printf("Only French and English are available.\n");
            printf("My English may contain mistakes.\n\n");
            printf("A: yes, keep English\n");
            printf("B: no, switch to French\n");
        }
        printf("%s\n", ui_text("START : QUITTER"));
    } else if (screen_mode == SCREEN_WELCOME) {
        printf("\n%s\n\n", ui_french ? "Bienvenue dans SHIELD Checker." :
                                  "Welcome to SHIELD Checker.");
        printf("%s\n", ui_french ? "Un controle lance tous les points." :
                               "One check runs through every item.");
        printf("%s\n", ui_french ? "Checker explique ce qu'il peut voir" :
                              "Checker explains what it can see");
        printf("%s\n\n", ui_french ? "et ce qu'il ne peut pas confirmer." :
                                "and what it cannot confirm.");
        printf("%s\n", ui_french ? "Un resultat indisponible ne veut" :
                              "An unavailable result does not");
        printf("%s\n\n", ui_french ? "pas dire que la 3DS est en panne." :
                                "mean your 3DS is broken.");
        if (ui_french) {
            printf("Aucun fichier systeme n'est modifie.\n");
            printf("Seule la langue choisie peut etre\n");
            printf("enregistree dans le dossier Checker.\n\n");
        } else {
            printf("No system file is changed. Only the\n");
            printf("chosen language may be saved in\n");
            printf("Checker's own folder.\n\n");
        }
        printf("%s\n", ui_french ? "A : ouvrir le menu" : "A: open menu");
        printf("%s", ui_text("B : RETOUR\n"));
    } else if (screen_mode == SCREEN_MENU) {
        printf("\n%s\n\n", ui_french ? "Choisis un parcours :" : "Choose a check:");
        for (size_t i = 0; i < MENU_COUNT; ++i)
            printf("%s %s\n\n", i == selected_menu ? ">" : " ",
                   ui_french ? menu_fr[i] : menu_en[i]);
        printf("%s\n", ui_french ? "Rapide : versions et logiciel." :
                               "Quick: versions and software.");
        printf("%s\n\n", ui_french ? "Avance : aussi le materiel." :
                                 "Advanced: also hardware.");
        printf("%s\n", ui_french ? "A : lancer   START : quitter" :
                               "A: start   START: quit");
        printf("%s\n", ui_french ? "Aucune modification systeme." :
                              "No system changes.");
    } else if (screen_mode == SCREEN_DIAGNOSTICS) {
        printf("\n%s\n\n", diagnostic_report.mode == DIAG_QUICK ?
               (ui_french ? "Diagnostic logiciel" : "Software results") :
               (ui_french ? "Logiciel et materiel" : "Software and hardware"));
        size_t first = selected_diagnostic > 3 ? selected_diagnostic - 3 : 0;
        if (diagnostic_report.count > 4 && first + 4 > diagnostic_report.count)
            first = diagnostic_report.count - 4;
        for (size_t i = first; i < diagnostic_report.count && i < first + 4; ++i) {
            DiagnosticItem *item = &diagnostic_report.items[i];
            printf("%s \x1b[36m%.24s\x1b[0m\n  %.37s\n\n", i == selected_diagnostic ? ">" : " ",
                   ui_french ? item->label_fr : item->label_en, item->value);
        }
        printf("%u/%u  %s\n", (unsigned)(selected_diagnostic + 1),
               (unsigned)diagnostic_report.count,
                ui_french ? "A: expliquer B: retour" : "A: explain B: back");
        printf("%s\n", ui_french ? "L: versions des apps" : "L: app versions");
        if (diagnostic_report.mode == DIAG_FULL)
            printf("%s\n", ui_french ? "X: touches Y: couleurs R: reseau" :
                                  "X: input Y: colors R: network");
    } else if (screen_mode == SCREEN_DIAG_DETAILS) {
        DiagnosticItem *item = &diagnostic_report.items[selected_diagnostic];
        printf("\n%s\n", ui_french ? item->label_fr : item->label_en);
        printf("%s: %.34s\n", ui_french ? "Observation" : "Observed", item->value);
        printf("%s\n\n", diagnostics_state_name(item->state, ui_french));
        printf("%s\n", ui_french ? "Ce que cela signifie :" : "What this means:");
        print_wrapped(ui_french ? item->explanation_fr : item->explanation_en, 38, 4);
        printf("\n");
        if (item->state == DIAG_UNAVAILABLE) {
            printf("%s\n", ui_french ? "Mesure non verifiee cette fois." :
                                  "Reading not verified this time.");
            printf("%s\n", ui_french ? "Cela ne prouve pas une panne." :
                                  "This does not prove a fault.");
        } else if (item->state == DIAG_NOT_DETECTED) {
            printf("%s\n", ui_french ? "Si tu l'attendais, verifie son" :
                                  "If you expected it, check its");
            printf("%s\n", ui_french ? "emplacement. Ce n'est pas une panne." :
                                  "location. This is not a fault.");
        } else if (item->state == DIAG_UNSUPPORTED) {
            printf("%s\n", ui_french ? "Aucune action n'est necessaire." :
                                  "No action is needed.");
        } else {
            printf("%s\n", ui_french ? "Ce resultat seul ne demande pas" :
                                  "This result alone does not");
            printf("%s\n", ui_french ? "d'action particuliere." :
                                  "call for any action.");
        }
        printf("\n");
        if (advanced_view) {
            printf("%s: %s\n", ui_french ? "Source" : "Source",
                   diagnostics_source(item));
            printf("%s: %u\n\n", ui_french ? "Code etat" : "State code",
                   (unsigned)item->state);
            if (strcmp(item->label_fr, "Connexion HTTPS") == 0 &&
                last_network_curl_code >= 0)
                printf("cURL %d / HTTP %ld\n\n", last_network_curl_code,
                       last_network_http_status);
        }
        printf("%s\n", ui_french ? "B: retour  HAUT/BAS: suivant" :
                              "B: back  UP/DOWN: next");
    } else if (screen_mode == SCREEN_TROUBLE_MENU) {
        printf("\n%s\n\n", ui_french ? "Quel probleme observes-tu ?" :
                                  "What problem do you see?");
        for (size_t i = 0; i < SYMPTOM_COUNT; ++i)
            printf("%s %s\n", i == selected_symptom ? ">" : " ",
                   ui_french ? symptom_fr[i] : symptom_en[i]);
        printf("\n%s\n", ui_french ? "A: examiner   B: menu" :
                              "A: inspect   B: menu");
    } else if (screen_mode == SCREEN_TROUBLE_DETAILS) {
        printf("\n%s\n\n", ui_french ? symptom_fr[selected_symptom] :
                                  symptom_en[selected_symptom]);
        print_troubleshooting();
    } else if (screen_mode == SCREEN_INPUT_TEST) {
        u32 held = hidKeysHeld();
        circlePosition circle;
        touchPosition touch;
        hidCircleRead(&circle);
        hidTouchRead(&touch);
        printf("\n%s\n\n", ui_french ? "Touches pressees :" : "Held buttons:");
        static const struct { u32 bit; const char *name; } buttons[] = {
            {KEY_A, "A"}, {KEY_B, "B"}, {KEY_X, "X"}, {KEY_Y, "Y"},
            {KEY_START, "START"}, {KEY_SELECT, "SELECT"},
            {KEY_DUP, "UP"}, {KEY_DDOWN, "DOWN"},
            {KEY_DLEFT, "LEFT"}, {KEY_DRIGHT, "RIGHT"},
            {KEY_L, "L"}, {KEY_R, "R"}
        };
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
            if (held & buttons[i].bit) printf("%s ", buttons[i].name);
        printf("\n\nCircle Pad: %d, %d\n", circle.dx, circle.dy);
        if (held & KEY_TOUCH) printf("Touch: %u, %u\n", touch.px, touch.py);
        else printf("Touch: --\n");
        printf("\n%s\n", ui_french ? "B: retour (non presse ne veut" :
                              "B: back (unpressed does not");
        printf("%s\n", ui_french ? "pas dire defectueux)" : "mean defective)");
    } else if (screen_mode == SCREEN_DISPLAY_TEST) {
        printf("\n%s\n\n", ui_french ? "Test visuel de l'ecran haut" :
                                  "Top screen visual test");
        printf("%s %u/5\n\n", ui_french ? "Couleur" : "Color", display_color + 1);
        printf("%s\n", ui_french ? "A : couleur suivante" : "A: next color");
        printf("%s\n\n", ui_french ? "B : retour" : "B: back");
        printf("%s\n", ui_french ? "X : tester l'ecran bas" :
                              "X: test bottom screen");
        printf("%s\n", ui_french ? "Observe toi-meme les pixels." :
                              "Inspect the pixels yourself.");
        printf("%s\n", ui_french ? "Aucun verdict automatique." :
                              "No automatic verdict.");
    } else if (screen_mode == SCREEN_SETTINGS) {
        printf("\n%s\n\n", ui_french ? "Reglages de l'interface" :
                                  "Interface settings");
        printf("%s: %s\n", ui_french ? "Affichage" : "View",
               advanced_view ? (ui_french ? "avance" : "advanced") :
                               (ui_french ? "simple" : "simple"));
        printf("%s: %s\n\n", ui_french ? "Langue" : "Language",
               ui_french ? "Francais" : "English");
        printf("A: %s\n", ui_french ? "changer la vue" : "toggle view");
        printf("X: %s\n", ui_french ? "changer la langue" : "switch language");
        printf("B: %s\n\n", ui_french ? "retour" : "back");
        printf("%s\n", ui_french ? "Seule la langue est enregistree" :
                              "Only the language is saved");
        printf("%s\n", ui_french ? "dans le dossier de Checker." :
                              "in Checker's own folder.");
    } else if (screen_mode == SCREEN_NETWORK_TEST) {
        printf("\n%s\n\n", ui_french ? "Test reseau en cours..." :
                                  "Network test running...");
        printf("HTTPS: api.github.com\n\n");
        printf("%s\n", ui_french ? "Aucune donnee personnelle envoyee." :
                              "No personal data sent.");
        printf("%s\n", ui_french ? "B : annuler" : "B: cancel");
    } else if (screen_mode == SCREEN_SCANNING) {
        printf("\x1b[36m%s\x1b[0m\n\n", ui_text("VERIFICATION EN COURS"));
        printf(ui_text("Etape %u/%u : %s\n\n"), scan_step, CHECK_COUNT, checks[active_check].name);
        printf("%s\n\n", checks[active_check].description);
        printf("%s\n", ui_french ? "Lecture de la carte SD." :
                              "Reading the SD card.");
        if (online_requested)
            printf("%s\n", ui_french ? "Consultation des versions en ligne." :
                                  "Checking versions online.");
        printf("%s", ui_text("Cela prend quelques instants.\n"));
        printf("%s", ui_text("Rien n'est installe ni modifie.\n"));
        printf("%s\n", ui_french ? "B : annuler le reseau" :
                              "B: cancel network check");
    } else if (screen_mode == SCREEN_RESULTS) {
        unsigned uncertain = 0;
        for (size_t i = 0; i < diagnostic_report.count; ++i)
            if (diagnostic_report.items[i].state == DIAG_UNAVAILABLE)
                ++uncertain;
        printf("\x1b[36m%s\x1b[0m\n\n",
               ui_french ? "RESULTATS DES OUTILS" : "TOOL RESULTS");
        for (size_t i = 0; i < CHECK_COUNT; ++i) {
            printf("%s %s\n", i == selected_check ? ">" : " ", checks[i].name);
            print_status_color(checks[i].status);
            printf("  %s\x1b[0m\n\n", status_label(&checks[i]));
        }
        printf("----------------------------------------\n");
        printf("%s: %u %s\n", ui_french ? "Diagnostic" : "Diagnostic",
               (unsigned)diagnostic_report.count,
               ui_french ? "points examines" : "items examined");
        printf("%u %s\n", uncertain, ui_french ? "non confirmes" : "unconfirmed");
        printf("%s\n", ui_french ? "A: expliquer  Y: diagnostic" :
                               "A: explain  Y: diagnostic");
        printf("%s\n", ui_french ? "X: reverifier  B: menu" :
                               "X: recheck  B: menu");
    } else {
        CheckEntry *entry = &checks[selected_check];
        printf("\x1b[36m%s\x1b[0m\n", entry->name);
        printf("%s", ui_text("Etat : "));
        print_status_color(entry->status);
        printf("%s\x1b[0m\n\n", status_label(entry));
        if (advanced_view) {
            printf("%s\n", ui_french ? "Emplacement examine :" : "Checked location:");
            print_wrapped(ui_text(entry->detected_path), 38, 2);
            if (entry->kind == CHECK_LUMA && entry->sd_file_checked)
                printf(ui_text("boot.firm sur la SD : %s\n"),
                       ui_text(entry->sd_file_present ? "present" : "absent"));
            if (entry->kind == CHECK_GODMODE9 && entry->support_dir_checked)
                printf(ui_text("Dossier gm9 (facultatif) : %s\n"),
                       ui_text(entry->support_dir_present ? "present" : "absent"));
            if (entry->kind == CHECK_CIA_VERSION_TITLE && entry->installed)
                printf("CIA interne : %u\n", entry->local_title_version);
            printf("%s\n", ui_french ? "Source officielle :" : "Official source:");
            print_wrapped(entry->api_url, 38, 3);
            printf("\n");
        }
        if (entry->kind == CHECK_LUMA && entry->sd_file_checked &&
            !entry->sd_file_present)
            printf("%s", ui_text("Luma peut aussi demarrer depuis CTRNAND.\n"));
        if (entry->installed && entry->kind == CHECK_CIA_VERSION_TITLE) {
            printf("%s\n", ui_french ? "Version locale : non lisible" :
                                  "Local version: not readable");
            printf("%s: %s\n\n", ui_french ? "Version publiee" :
                                    "Published version", entry->latest_text);
        } else if (entry->installed)
            printf(ui_text("%s : %s\nDerniere version : %s\n\n"),
                   ui_text(entry->kind == CHECK_LUMA ? "Version active" : "Version sur SD"),
                   entry->local_text, entry->latest_text);
        print_status_explanation(entry);
        printf("%s", ui_text("\nB : RETOUR   HAUT/BAS : COMPOSANT\n"));
        printf("%s\n", ui_french ? "X: verifier en ligne  START: quitter" :
                              "X: check online  START: quit");
    }
}

static void refresh_ui(void) {
    draw_bottom();
    draw_top();
    gfxFlushBuffers();
    gfxSwapBuffers();
}

static bool confirm_ui_language(void) {
    if (!language_confirmation_needed) return true;
    screen_mode = SCREEN_LANGUAGE;
    refresh_ui();
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) {
            return false;
        }
        if (down & KEY_A) {
            save_ui_language();
            language_confirmation_needed = false;
            screen_mode = SCREEN_WELCOME;
            reset_check_entries();
            refresh_ui();
            return true;
        }
        if (down & KEY_B) {
            ui_french = !ui_french;
            save_ui_language();
            language_confirmation_needed = false;
            screen_mode = SCREEN_WELCOME;
            reset_check_entries();
            refresh_ui();
            return true;
        }
        gspWaitForVBlank();
    }
    return false;
}

static bool load_romfs_assets(void) {
    FILE *file = fopen("romfs:/font.bin", "rb");
    if (file) {
        font_atlas = malloc(95 * 160);
        if (font_atlas && fread(font_atlas, 1, 95 * 160, file) != 95 * 160) {
            free(font_atlas);
            font_atlas = NULL;
        }
        fclose(file);
    }
    return load_ca_bundle();
}

static void show_boot_splash(void) {
    FILE *file = fopen("romfs:/boot-top.bgr", "rb");
    if (!file) return;
    u8 *frame = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (frame && fread(frame, 1, 400 * 240 * 3, file) == 400 * 240 * 3) {
        // The raw artwork is already in the top framebuffer's BGR layout.
        // The lower screen keeps the normal welcome text during this pause.
        draw_bottom();
        gfxFlushBuffers();
        gfxSwapBuffers();
        svcSleepThread(1000000000LL);
    }
    fclose(file);
}

static void cleanup(void) {
    if (curl_ready) curl_global_cleanup();
    if (service_soc) socExit();
    if (service_am) amExit();
    free(socket_buffer);
    free(sd_title_ids);
    free(ca_bundle);
    free(font_atlas);
    if (service_romfs) romfsExit();
    gfxExit();
}

int main(void) {
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    consoleInit(GFX_BOTTOM, NULL);
    load_ui_language();
    hidScanInput();
    if (hidKeysHeld() & KEY_X)
        language_confirmation_needed = true;
    reset_check_entries();
    service_am = R_SUCCEEDED(amInit());
    service_romfs = R_SUCCEEDED(romfsInit());
    if (service_romfs) {
        load_romfs_assets();
    }
    screen_mode = SCREEN_WELCOME;
    if (service_romfs) show_boot_splash();
    if (!confirm_ui_language()) {
        cleanup();
        return 0;
    }
    refresh_ui();
    refresh_ui();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if ((down & KEY_START) && screen_mode != SCREEN_INPUT_TEST) break;
        if (screen_mode == SCREEN_WELCOME) {
            if (down & KEY_B) break;
            if (down & KEY_A) {
                screen_mode = SCREEN_MENU;
                refresh_ui();
            }
        } else if (screen_mode == SCREEN_MENU) {
            if (down & KEY_DUP) {
                selected_menu = (selected_menu + MENU_COUNT - 1) % MENU_COUNT;
                refresh_ui();
            } else if (down & KEY_DDOWN) {
                selected_menu = (selected_menu + 1) % MENU_COUNT;
                refresh_ui();
            } else if (down & KEY_A) {
                if (selected_menu <= 1) {
                    online_requested = true;
                    bool online = service_romfs && ca_bundle && init_network();
                    run_checks(online);
                    diagnostics_collect(&diagnostic_report,
                                        selected_menu == 0 ? DIAG_QUICK : DIAG_FULL,
                                        service_am);
                    selected_diagnostic = 0;
                    have_results = true;
                    last_result_screen = SCREEN_RESULTS;
                    refresh_ui();
                } else if (selected_menu == 2) {
                    diagnostics_collect(&diagnostic_report, DIAG_FULL, service_am);
                    selected_symptom = 0;
                    screen_mode = SCREEN_TROUBLE_MENU;
                    refresh_ui();
                } else if (selected_menu == 3) {
                    if (!have_results) {
                        online_requested = true;
                        bool online = service_romfs && ca_bundle && init_network();
                        run_checks(online);
                        diagnostics_collect(&diagnostic_report, DIAG_QUICK, service_am);
                        selected_diagnostic = 0;
                        have_results = true;
                        last_result_screen = SCREEN_RESULTS;
                    }
                    screen_mode = last_result_screen;
                    refresh_ui();
                } else {
                    screen_mode = SCREEN_SETTINGS;
                    refresh_ui();
                }
            } else if (down & KEY_B) {
                screen_mode = SCREEN_WELCOME;
                refresh_ui();
            }
        } else if (screen_mode == SCREEN_DIAGNOSTICS ||
                   screen_mode == SCREEN_DIAG_DETAILS) {
            if (down & KEY_DUP) {
                selected_diagnostic = (selected_diagnostic + diagnostic_report.count - 1) %
                                      diagnostic_report.count;
                refresh_ui();
            } else if (down & KEY_DDOWN) {
                selected_diagnostic = (selected_diagnostic + 1) % diagnostic_report.count;
                refresh_ui();
            } else if (down & KEY_B) {
                screen_mode = screen_mode == SCREEN_DIAG_DETAILS ?
                              SCREEN_DIAGNOSTICS : SCREEN_RESULTS;
                refresh_ui();
            } else if (down & KEY_A) {
                screen_mode = SCREEN_DIAG_DETAILS;
                refresh_ui();
            } else if (screen_mode == SCREEN_DIAGNOSTICS && (down & KEY_L)) {
                screen_mode = SCREEN_RESULTS;
                refresh_ui();
            } else if (screen_mode == SCREEN_DIAGNOSTICS &&
                       diagnostic_report.mode == DIAG_FULL && (down & KEY_X)) {
                test_return_screen = SCREEN_DIAGNOSTICS;
                screen_mode = SCREEN_INPUT_TEST;
                refresh_ui();
            } else if (screen_mode == SCREEN_DIAGNOSTICS &&
                       diagnostic_report.mode == DIAG_FULL && (down & KEY_Y)) {
                test_return_screen = SCREEN_DIAGNOSTICS;
                display_color = 0;
                display_lower = false;
                screen_mode = SCREEN_DISPLAY_TEST;
                refresh_ui();
            } else if (screen_mode == SCREEN_DIAGNOSTICS &&
                       diagnostic_report.mode == DIAG_FULL && (down & KEY_R)) {
                run_network_probe();
            }
        } else if (screen_mode == SCREEN_TROUBLE_MENU) {
            if (down & KEY_DUP) {
                selected_symptom = (selected_symptom + SYMPTOM_COUNT - 1) % SYMPTOM_COUNT;
                refresh_ui();
            } else if (down & KEY_DDOWN) {
                selected_symptom = (selected_symptom + 1) % SYMPTOM_COUNT;
                refresh_ui();
            } else if (down & KEY_A) {
                screen_mode = SCREEN_TROUBLE_DETAILS;
                refresh_ui();
            } else if (down & KEY_B) {
                screen_mode = SCREEN_MENU;
                refresh_ui();
            }
        } else if (screen_mode == SCREEN_TROUBLE_DETAILS) {
            if (down & KEY_B) {
                screen_mode = SCREEN_TROUBLE_MENU;
                refresh_ui();
            } else if (selected_symptom == 5 && (down & KEY_X)) {
                test_return_screen = SCREEN_TROUBLE_DETAILS;
                screen_mode = SCREEN_INPUT_TEST;
                refresh_ui();
            } else if (selected_symptom == 6 && (down & KEY_Y)) {
                test_return_screen = SCREEN_TROUBLE_DETAILS;
                display_color = 0;
                display_lower = false;
                screen_mode = SCREEN_DISPLAY_TEST;
                refresh_ui();
            }
        } else if (screen_mode == SCREEN_INPUT_TEST) {
            if (down & KEY_B) {
                screen_mode = test_return_screen;
                refresh_ui();
            } else {
                static u64 last_input_refresh;
                u64 now = osGetTime();
                if (now - last_input_refresh >= 100) {
                    last_input_refresh = now;
                    refresh_ui();
                }
            }
        } else if (screen_mode == SCREEN_DISPLAY_TEST) {
            if (down & KEY_B) {
                screen_mode = test_return_screen;
                refresh_ui();
            } else if (down & KEY_A) {
                display_color = (display_color + 1) % 5;
                refresh_ui();
            } else if (down & KEY_X) {
                display_lower = !display_lower;
                refresh_ui();
            }
        } else if (screen_mode == SCREEN_SETTINGS) {
            if (down & KEY_B) {
                screen_mode = SCREEN_MENU;
                refresh_ui();
            } else if (down & KEY_A) {
                advanced_view = !advanced_view;
                refresh_ui();
            } else if (down & KEY_X) {
                ui_french = !ui_french;
                save_ui_language();
                refresh_ui();
            }
        } else if (screen_mode == SCREEN_RESULTS) {
            bool ui_changed = false;
            if (down & KEY_B) { screen_mode = SCREEN_MENU; ui_changed = true; }
            if (down & KEY_DUP) { selected_check = (selected_check + CHECK_COUNT - 1) % CHECK_COUNT; ui_changed = true; }
            if (down & KEY_DDOWN) { selected_check = (selected_check + 1) % CHECK_COUNT; ui_changed = true; }
            if (down & KEY_A) { screen_mode = SCREEN_DETAILS; ui_changed = true; }
            if ((down & KEY_Y) && diagnostic_report.count) {
                screen_mode = SCREEN_DIAGNOSTICS;
                selected_diagnostic = 0;
                ui_changed = true;
            }
            if (down & KEY_X) {
                online_requested = true;
                bool online = service_romfs && ca_bundle && init_network();
                run_checks(online);
                ui_changed = false;
            }
            if (ui_changed) refresh_ui();
        } else if (screen_mode == SCREEN_DETAILS) {
            bool ui_changed = false;
            if (down & KEY_B) { screen_mode = SCREEN_RESULTS; ui_changed = true; }
            if (down & KEY_DUP) { selected_check = (selected_check + CHECK_COUNT - 1) % CHECK_COUNT; ui_changed = true; }
            if (down & KEY_DDOWN) { selected_check = (selected_check + 1) % CHECK_COUNT; ui_changed = true; }
            if (down & KEY_X) {
                screen_mode = SCREEN_SCANNING;
                online_requested = true;
                bool online = service_romfs && ca_bundle && init_network();
                run_checks(online);
                ui_changed = false;
            }
            if (ui_changed) refresh_ui();
        }
        gspWaitForVBlank();
    }
    cleanup();
    return 0;
}
