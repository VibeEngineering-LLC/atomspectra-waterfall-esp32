// AWF-1 (#2): выбор источника восстановления (main/spectrum_restore_plan.h).
#include "spectrum_restore_plan.h"
#include "test_util.h"

void test_restore_plan(void)
{
    CHECK(restore_pick_source(true,  true,  true)  == RESTORE_SRC_MAIN);
    CHECK(restore_pick_source(true,  false, false) == RESTORE_SRC_MAIN);
    CHECK(restore_pick_source(false, true,  true)  == RESTORE_SRC_TMP);
    CHECK(restore_pick_source(false, true,  false) == RESTORE_SRC_TMP);
    CHECK(restore_pick_source(false, false, true)  == RESTORE_SRC_BACKUP);
    CHECK(restore_pick_source(false, false, false) == RESTORE_SRC_NONE);
}
