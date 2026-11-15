#ifndef SHIELD_DIAGNOSTICS_H
#define SHIELD_DIAGNOSTICS_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#define DIAG_MAX_ITEMS 20

typedef enum {
    DIAG_OK,
    DIAG_INFO,
    DIAG_NOT_DETECTED,
    DIAG_WARNING,
    DIAG_UNAVAILABLE,
    DIAG_UNSUPPORTED
} DiagnosticState;

typedef enum {
    DIAG_QUICK,
    DIAG_STANDARD,
    DIAG_FULL
} DiagnosticMode;

typedef struct {
    const char *label_fr;
    const char *label_en;
    const char *explanation_fr;
    const char *explanation_en;
    char value[48];
    DiagnosticState state;
} DiagnosticItem;

typedef struct {
    DiagnosticItem items[DIAG_MAX_ITEMS];
    size_t count;
    DiagnosticMode mode;
} DiagnosticReport;

/* All probes are read-only and run once when the user starts a diagnostic. */
void diagnostics_collect(DiagnosticReport *report, DiagnosticMode mode, bool am_ready);
void diagnostics_set_network(DiagnosticReport *report, const char *value,
                             DiagnosticState state, const char *detail_fr,
                             const char *detail_en);
const char *diagnostics_state_name(DiagnosticState state, bool french);
const char *diagnostics_source(const DiagnosticItem *item);

#endif
