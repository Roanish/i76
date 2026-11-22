/* fidelity.c — see fidelity.h. */
#include "engine/fidelity.h"

static bool s_legacy;

void fidelity_set_legacy(bool on) { s_legacy = on; }
bool fidelity_legacy(void)        { return s_legacy; }
