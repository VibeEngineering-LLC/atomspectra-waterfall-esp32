// AWF-2a финал: boot_flag_from_nvs (main/boot_flag_plan.h) — семантика
// отсутствующего ключа. Критично для field_ap_fallback_enabled: решение
// оператора 25.09 — отсутствующий ключ обязан дать ВЫКЛ.
#include "boot_flag_plan.h"
#include "test_util.h"

void test_boot_flag_plan(void)
{
    // Ключа нет в NVS (апгрейд со старой прошивки, где настройки не было) —
    // ВЫКЛ, вне зависимости от мусора в raw_value (не должен читаться).
    CHECK(!boot_flag_from_nvs(false, 0));
    CHECK(!boot_flag_from_nvs(false, 1));
    CHECK(!boot_flag_from_nvs(false, 0xFF));

    // Ключ есть, значение 0 — явно выключено оператором.
    CHECK(!boot_flag_from_nvs(true, 0));
    // Ключ есть, значение != 0 — включено.
    CHECK(boot_flag_from_nvs(true, 1));
    CHECK(boot_flag_from_nvs(true, 0xFF));
}
