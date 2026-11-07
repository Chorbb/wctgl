/* xlat_none.c — пустой набор переведённых процедур: ядро без перевода (эталон, покрытие кода). */
#include "xlat.h"
const XlatInfo xlat_table[1] = { { 0, 0, 0, 0, 0, 0 } };
const int xlat_count = 0;
void xlat_register_all(Nes *n) { (void)n; }
