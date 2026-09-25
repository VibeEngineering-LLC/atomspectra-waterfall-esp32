// AWF-3: обнаружение сброса прибора и слияние база+прибор (spectrum_base_plan.h).
#include "spectrum_base_plan.h"
#include "test_util.h"

static void test_reset_detection(void)
{
    // Живой сценарий 25.09: D=4451с, база=0, первый STAT после обрыва
    // питания t=11с — обязан быть распознан как сброс.
    CHECK(spectrum_base_reset_detected(11, 0, 4451));
    // Обычная работа: прибор продолжает копить — НЕ сброс.
    CHECK(!spectrum_base_reset_detected(101, 0, 100));
    // После fold (база=50, показано=53): прибор продолжает нормально — НЕ сброс.
    CHECK(!spectrum_base_reset_detected(54, 50, 53));

    // Граница допуска (5с): dev_elapsed_since_base=100, dev=95 -> 95+5=100 —
    // ещё НЕ сброс (легитимная просадка STAT, #FW-12); dev=94 — уже сброс.
    CHECK(!spectrum_base_reset_detected(95, 0, 100));
    CHECK(spectrum_base_reset_detected(94, 0, 100));
    // Равенство времени — НЕ сброс (мутация "без допуска"/"<=" ловится тут же).
    CHECK(!spectrum_base_reset_detected(100, 0, 100));

    // Шлюз перезагрузился, прибор — нет: база=50, D=100 (прибор был на 50),
    // новый STAT прибора t=53 (жил ещё пару c, пока шлюз поднимался) -> НЕ сброс.
    CHECK(!spectrum_base_reset_detected(53, 50, 100));
}

static void test_merge(void)
{
    CHECK(spectrum_base_merge(0, 11) == 11);
    CHECK(spectrum_base_merge(4451, 11) == 4462);
    CHECK(spectrum_base_merge(727021, 1810) == 728831);
    CHECK(spectrum_base_merge(0, 0) == 0);
}

// Сценарий целиком: старт с восстановленного D (4451с) -> первый STAT прибора
// (11с) распознан как сброс -> база=D, показано=D+прибор; затем обычный рост
// прибора без сброса; затем очистка (всё по нулям).
static void test_sequence(void)
{
    uint32_t base_time = 0, base_bins[3] = {0, 0, 0};
    uint32_t shown_time = 4451, shown_bins[3] = {100, 200, 300};

    uint32_t dev_bins[3] = {1, 0, 2};
    CHECK(spectrum_base_reset_detected(11, base_time, shown_time));
    for (int i = 0; i < 3; i++) base_bins[i] = shown_bins[i];   // база = ПОКАЗАННЫЙ спектр
    base_time = shown_time;
    for (int i = 0; i < 3; i++) shown_bins[i] = spectrum_base_merge(base_bins[i], dev_bins[i]);
    shown_time = spectrum_base_merge(base_time, 11);
    CHECK(shown_bins[0] == 101 && shown_bins[2] == 302 && shown_time == 4462);

    uint32_t dev_bins2[3] = {5, 0, 2};
    CHECK(!spectrum_base_reset_detected(15, base_time, shown_time));
    for (int i = 0; i < 3; i++) shown_bins[i] = spectrum_base_merge(base_bins[i], dev_bins2[i]);
    shown_time = spectrum_base_merge(base_time, 15);
    CHECK(base_time == 4451);   // база НЕ менялась
    CHECK(shown_bins[0] == 105 && shown_time == 4466);

    base_time = 0; shown_time = 0;
    for (int i = 0; i < 3; i++) { base_bins[i] = 0; shown_bins[i] = 0; }
    CHECK(base_time == 0 && shown_time == 0 && shown_bins[0] == 0 && shown_bins[2] == 0);
}

void spectrum_base_plan_suite(void)
{
    test_reset_detection();
    test_merge();
    test_sequence();
}
