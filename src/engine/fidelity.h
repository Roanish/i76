#ifndef FIDELITY_H
#define FIDELITY_H

#include <stdbool.h>

/*
 * fidelity.h — the faithful ("legacy") alternative to every deviation.
 *
 * Project rule: we reproduce the original's method wherever possible. Where we
 * deviate, the deviation is listed in docs/DEVIATIONS.md, and the original
 * behaviour stays available behind this switch (command line: --legacy).
 * Code that deviates checks fidelity_legacy() and runs the original path when
 * it is set.
 */

void fidelity_set_legacy(bool on);
bool fidelity_legacy(void);

#endif /* FIDELITY_H */
