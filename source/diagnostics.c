#include "diagnostics.h"
#include <3ds/services/am.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static void add(DiagnosticReport *report, const char *fr, const char *en,
                const char *detail_fr, const char *detail_en,
                const char *value, DiagnosticState state) {
    if (report->count >= DIAG_MAX_ITEMS) return;
    DiagnosticItem *item = &report->items[report->count++];
    item->label_fr = fr;
    item->label_en = en;
    item->explanation_fr = detail_fr;
    item->explanation_en = detail_en;
    snprintf(item->value, sizeof(item->value), "%s", value);
    item->state = state;
}

const char *diagnostics_state_name(DiagnosticState state, bool french) {
    switch (state) {
        case DIAG_OK: return "OK";
        case DIAG_INFO: return french ? "INFORMATION" : "INFORMATION";
        case DIAG_NOT_DETECTED: return french ? "NON DETECTE" : "NOT DETECTED";
        case DIAG_WARNING: return french ? "A VERIFIER" : "CHECK";
        case DIAG_UNAVAILABLE: return french ? "NON VERIFIE" : "NOT VERIFIED";
        case DIAG_UNSUPPORTED: return french ? "NON PRIS EN CHARGE" : "NOT SUPPORTED";
        default: return "?";
    }
}

const char *diagnostics_source(const DiagnosticItem *item) {
    if (!item) return "--";
    const char *label = item->label_fr;
    if (strcmp(label, "Modele") == 0) return "CFGU_GetSystemModel";
    if (strcmp(label, "Version systeme") == 0) return "NVer/CVer";
    if (strcmp(label, "RAM region app") == 0) return "svcGetSystemInfo";
    if (strcmp(label, "Luma actif") == 0) return "Luma system info";
    if (strcmp(label, "GodMode9 SD") == 0 || strcmp(label, "Dossier /3ds") == 0)
        return "SD stat()";
    if (strcmp(label, "Universal Updater") == 0 ||
        strcmp(label, "Shield Updater") == 0 || strcmp(label, "hShop / 3hs") == 0)
        return "AM_GetTitleList (SD)";
    if (strcmp(label, "Batterie") == 0 || strcmp(label, "Charge") == 0 ||
        strcmp(label, "Adaptateur") == 0) return "PTMU";
    if (strcmp(label, "Carte SD") == 0 || strcmp(label, "Espace SD") == 0)
        return "FSUSER";
    if (strcmp(label, "Curseur volume") == 0) return "HIDUSER";
    if (strcmp(label, "Casque") == 0 || strcmp(label, "Signal Wi-Fi") == 0 ||
        strcmp(label, "Curseur 3D") == 0) return "OS shared config";
    if (strcmp(label, "Connexion HTTPS") == 0) return "libcurl HTTPS";
    return "--";
}

static const char *model_name(u8 model) {
    switch (model) {
        case CFG_MODEL_3DS: return "Old 3DS";
        case CFG_MODEL_3DSXL: return "Old 3DS XL";
        case CFG_MODEL_2DS: return "2DS";
        case CFG_MODEL_N3DS: return "New 3DS";
        case CFG_MODEL_N3DSXL: return "New 3DS XL";
        case CFG_MODEL_N2DSXL: return "New 2DS XL";
        default: return "Unknown";
    }
}

static void probe_system(DiagnosticReport *report) {
    u8 model = 0;
    if (R_SUCCEEDED(cfguInit())) {
        bool known = R_SUCCEEDED(CFGU_GetSystemModel(&model)) &&
                     model <= CFG_MODEL_N2DSXL;
        add(report, "Modele", "Model",
            "Modele declare par le service systeme.",
            "Model reported by the system service.",
            known ? model_name(model) : "--", known ? DIAG_INFO : DIAG_UNAVAILABLE);
        cfguExit();
    } else {
        add(report, "Modele", "Model", "Service indisponible.",
            "Service unavailable.", "--", DIAG_UNAVAILABLE);
    }

    OS_VersionBin nver, cver;
    char version[32] = {0};
    bool have_version = R_SUCCEEDED(osGetSystemVersionDataString(
        &nver, &cver, version, sizeof(version)));
    add(report, "Version systeme", "System version",
        "Version lue depuis NVer/CVer; aucune modification.",
        "Version read from NVer/CVer; no changes.",
        have_version ? version : "--", have_version ? DIAG_INFO : DIAG_UNAVAILABLE);

    u32 app_size = osGetMemRegionSize(MEMREGION_APPLICATION);
    s64 app_used = 0;
    bool have_used = R_SUCCEEDED(svcGetSystemInfo(&app_used, 0, MEMREGION_APPLICATION));
    char memory[48];
    bool sane = have_used && app_size > 0 && app_used >= 0 &&
                (u64)app_used <= app_size;
    if (sane) snprintf(memory, sizeof(memory), "%lu / %lu MiB",
                       (unsigned long)((app_size - (u32)app_used) / 1048576),
                       (unsigned long)(app_size / 1048576));
    add(report, "RAM region app", "App memory region",
        "Libre / total dans la region application; pas la RAM physique.",
        "Free / total in app memory region; not physical RAM.",
        sane ? memory : "--", sane ? DIAG_INFO : DIAG_UNAVAILABLE);
}

static void probe_software(DiagnosticReport *report, bool am_ready) {
    s64 packed_info = 0;
    bool have_luma = R_SUCCEEDED(svcGetSystemInfo(&packed_info, 0x10000, 0));
    u32 packed = (u32)packed_info;
    unsigned major = (packed >> 24) & 0xFF;
    unsigned minor = (packed >> 16) & 0xFF;
    unsigned patch = (packed >> 8) & 0xFF;
    have_luma = have_luma && major > 0 && major <= 99 && minor <= 99 && patch <= 99;
    char value[48];
    if (have_luma) snprintf(value, sizeof(value), "%u.%u.%u", major, minor, patch);
    add(report, "Luma actif", "Active Luma",
        "Version active signalee par Luma; pas celle du fichier SD.",
        "Active version reported by Luma, not the SD file.",
        have_luma ? value : "--", have_luma ? DIAG_INFO : DIAG_UNAVAILABLE);

    static const char *gm9_paths[] = {
        "sdmc:/luma/payloads/GodMode9.firm",
        "sdmc:/gm9/payloads/GodMode9.firm",
        "sdmc:/GodMode9.firm", "sdmc:/gm9/GodMode9.firm"
    };
    bool gm9_found = false;
    bool gm9_uncertain = false;
    for (size_t i = 0; i < sizeof(gm9_paths) / sizeof(gm9_paths[0]); ++i) {
        struct stat info;
        int result = stat(gm9_paths[i], &info);
        if (result == 0) {
            if (S_ISREG(info.st_mode)) {
                gm9_found = true;
                break;
            }
            gm9_uncertain = true;
        } else if (errno != ENOENT && errno != ENOTDIR) {
            gm9_uncertain = true;
        }
    }
    add(report, "GodMode9 SD", "GodMode9 on SD",
        "Recherche dans quatre chemins; absence non critique.",
        "Four paths checked; absence is not a fault.",
        gm9_found ? "Trouve / Found" :
        gm9_uncertain ? "--" : "Non trouve / Not found",
        gm9_uncertain && !gm9_found ? DIAG_UNAVAILABLE :
        gm9_found ? DIAG_INFO : DIAG_NOT_DETECTED);

    static const struct { u64 id; const char *fr; const char *en; } apps[] = {
        {UINT64_C(0x0004000004391700), "Universal Updater", "Universal Updater"},
        {UINT64_C(0x000400000A210400), "Shield Updater", "Shield Updater"},
        {UINT64_C(0x0004000003DF1000), "hShop / 3hs", "hShop / 3hs"}
    };
    bool titles_known = false;
    u64 *ids = NULL;
    u32 count = 0, read_count = 0;
    if (am_ready) {
        if (R_SUCCEEDED(AM_GetTitleCount(MEDIATYPE_SD, &count)) && count <= 4096) {
            if (count == 0) {
                titles_known = true;
            } else if (count <= SIZE_MAX / sizeof(*ids)) {
                ids = malloc((size_t)count * sizeof(*ids));
                if (ids && R_SUCCEEDED(AM_GetTitleList(&read_count, MEDIATYPE_SD, count, ids)) &&
                    read_count <= count) titles_known = true;
            }
        }
    }
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); ++i) {
        bool found = false;
        for (u32 j = 0; titles_known && j < read_count; ++j)
            if (ids[j] == apps[i].id) { found = true; break; }
        add(report, apps[i].fr, apps[i].en,
            "Presence parmi les titres installes sur SD; version dans Outils.",
            "Presence among SD titles; version in Tools.",
            titles_known ? (found ? "Installe / Installed" : "Non trouve / Not found") : "--",
            titles_known ? (found ? DIAG_INFO : DIAG_NOT_DETECTED) :
                           DIAG_UNAVAILABLE);
    }
    free(ids);
}

static void probe_battery(DiagnosticReport *report) {
    if (R_FAILED(ptmuInit())) {
        add(report, "Batterie", "Battery", "Service batterie indisponible.",
            "Battery service unavailable.", "--", DIAG_UNAVAILABLE);
        return;
    }
    u8 level = 0, charging = 0;
    bool adapter = false;
    char value[48];
    bool have_level = R_SUCCEEDED(PTMU_GetBatteryLevel(&level)) && level <= 5;
    if (have_level) snprintf(value, sizeof(value), "%u/5", (unsigned)level);
    add(report, "Batterie", "Battery",
        "Niveau par paliers de 0 a 5; ce n'est pas un pourcentage.",
        "Level in steps from 0 to 5; not a percentage.",
        have_level ? value : "--", have_level ? DIAG_INFO : DIAG_UNAVAILABLE);
    bool have_charge = R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging)) &&
                       charging <= 1;
    add(report, "Charge", "Charging",
        "Etat de charge signale par la console.",
        "Charging state reported by the console.",
        have_charge ? (charging ? "Oui / Yes" : "Non / No") : "--",
        have_charge ? DIAG_INFO : DIAG_UNAVAILABLE);
    bool have_adapter = R_SUCCEEDED(PTMU_GetAdapterState(&adapter));
    add(report, "Adaptateur", "Power adapter",
        "Presence du chargeur; ne mesure pas la sante de la batterie.",
        "Adapter presence; does not measure battery health.",
        have_adapter ? (adapter ? "Connecte / Connected" : "Absent") : "--",
        have_adapter ? DIAG_INFO : DIAG_UNAVAILABLE);
    ptmuExit();
}

static void probe_storage(DiagnosticReport *report, DiagnosticMode mode) {
    bool detected = false;
    bool known = R_SUCCEEDED(FSUSER_IsSdmcDetected(&detected));
    add(report, "Carte SD", "SD card",
        "Detection signalee par le service de fichiers.",
        "Detection reported by the file service.",
        known ? (detected ? "Detectee / Detected" : "Absente / Missing") : "--",
        known ? (detected ? DIAG_OK : DIAG_NOT_DETECTED) : DIAG_UNAVAILABLE);
    if (!known || !detected || mode == DIAG_QUICK) return;

    FS_ArchiveResource resource;
    bool have_space = R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&resource)) &&
                      resource.clusterSize > 0 &&
                      resource.freeClusters <= resource.totalClusters;
    char space[48];
    if (have_space) {
        unsigned long long free_mb =
            (unsigned long long)resource.freeClusters * resource.clusterSize / 1048576;
        unsigned long long total_mb =
            (unsigned long long)resource.totalClusters * resource.clusterSize / 1048576;
        snprintf(space, sizeof(space), "%llu / %llu MiB", free_mb, total_mb);
    }
    add(report, "Espace SD", "SD free space",
        "Libre / total selon le service de fichiers.",
        "Free / total reported by the file service.",
        have_space ? space : "--", have_space ? DIAG_INFO : DIAG_UNAVAILABLE);

    struct stat info;
    int folder_result = stat("sdmc:/3ds", &info);
    bool homebrew = folder_result == 0 && S_ISDIR(info.st_mode);
    bool folder_known = homebrew ||
                        (folder_result < 0 && (errno == ENOENT || errno == ENOTDIR));
    add(report, "Dossier /3ds", "/3ds folder",
        "Dossier homebrew courant; son absence n'est pas une panne.",
        "Common homebrew folder; absence is not a fault.",
        homebrew ? "Present" : folder_known ? "Non trouve / Not found" : "--",
        folder_known ? (homebrew ? DIAG_INFO : DIAG_NOT_DETECTED) :
                       DIAG_UNAVAILABLE);
}

static void probe_full(DiagnosticReport *report) {
    u8 slider = 0;
    bool volume_known = R_SUCCEEDED(HIDUSER_GetSoundVolume(&slider)) && slider <= 63;
    char volume[48];
    if (volume_known) snprintf(volume, sizeof(volume), "%u/63", (unsigned)slider);
    add(report, "Curseur volume", "Volume slider",
        "Position du curseur; ne prouve pas la sortie audio physique.",
        "Slider position; does not prove physical audio output.",
        volume_known ? volume : "--", volume_known ? DIAG_INFO : DIAG_UNAVAILABLE);
    add(report, "Casque", "Headset",
        "Detection de prise; ne prouve pas que le casque fonctionne.",
        "Jack detection; does not prove the headset works.",
        osIsHeadsetConnected() ? "Connecte / Connected" : "Absent", DIAG_INFO);
    u8 wifi = osGetWifiStrength();
    char signal[48];
    bool wifi_known = wifi <= 3;
    if (wifi_known) snprintf(signal, sizeof(signal), "%u/3", (unsigned)wifi);
    add(report, "Signal Wi-Fi", "Wi-Fi signal",
        "Barres radio; 0 ne prouve pas une panne materielle.",
        "Radio bars; 0 does not prove a hardware fault.",
        wifi_known ? signal : "--", wifi_known ? DIAG_INFO : DIAG_UNAVAILABLE);
    u8 model = 0;
    bool model_known = false;
    if (R_SUCCEEDED(cfguInit())) {
        model_known = R_SUCCEEDED(CFGU_GetSystemModel(&model)) &&
                      model <= CFG_MODEL_N2DSXL;
        cfguExit();
    }
    float slider_3d = osGet3DSliderState();
    bool has_3d = model_known && model != CFG_MODEL_2DS && model != CFG_MODEL_N2DSXL;
    bool valid_slider = slider_3d >= 0.0f && slider_3d <= 1.0f;
    char slider_text[48];
    if (has_3d && valid_slider)
        snprintf(slider_text, sizeof(slider_text), "%u/100",
                 (unsigned)(slider_3d * 100.0f + 0.5f));
    add(report, "Curseur 3D", "3D slider",
        "Position du curseur; ne prouve pas la qualite de l'ecran.",
        "Slider position; does not prove screen quality.",
        !model_known ? "--" : !has_3d ? "Sans 3D / No 3D" :
        valid_slider ? slider_text : "--",
        !model_known ? DIAG_UNAVAILABLE : !has_3d ? DIAG_UNSUPPORTED :
        valid_slider ? DIAG_INFO : DIAG_UNAVAILABLE);
    add(report, "Temperatures", "Temperatures",
        "Aucune mesure CPU ou batterie fiable via ces interfaces.",
        "No reliable CPU or battery reading from these interfaces.",
        "--", DIAG_UNSUPPORTED);
}

void diagnostics_collect(DiagnosticReport *report, DiagnosticMode mode, bool am_ready) {
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->mode = mode;
    probe_software(report, am_ready);
    if (mode == DIAG_QUICK) return;
    probe_system(report);
    probe_battery(report);
    probe_storage(report, mode);
    if (mode == DIAG_FULL) probe_full(report);
}

void diagnostics_set_network(DiagnosticReport *report, const char *value,
                             DiagnosticState state, const char *detail_fr,
                             const char *detail_en) {
    if (!report) return;
    for (size_t i = 0; i < report->count; ++i) {
        if (strcmp(report->items[i].label_fr, "Connexion HTTPS") == 0) {
            report->items[i].state = state;
            report->items[i].explanation_fr = detail_fr;
            report->items[i].explanation_en = detail_en;
            snprintf(report->items[i].value, sizeof(report->items[i].value), "%s", value);
            return;
        }
    }
    add(report, "Connexion HTTPS", "HTTPS connection", detail_fr, detail_en,
        value, state);
}
