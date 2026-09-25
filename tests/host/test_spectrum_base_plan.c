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

// P2: насыщение на границе UINT32_MAX.
static void test_merge_overflow(void)
{
    CHECK(spectrum_base_merge(0xFFFFFFFFu, 1) == 0xFFFFFFFFu);
    CHECK(spectrum_base_merge(0xFFFFFFF0u, 0x20) == 0xFFFFFFFFu);
    CHECK(spectrum_base_merge(0xFFFFFFFFu, 0) == 0xFFFFFFFFu);
    CHECK(spectrum_base_merge(100, 200) == 300);
}

// P1-b: сброс по СЧЁТУ, независимо от STAT.
static void test_counts_reset(void)
{
    CHECK(spectrum_base_reset_detected_by_counts(9, 0, 267049));       // живой сценарий 25.09
    CHECK(!spectrum_base_reset_detected_by_counts(267049, 0, 267049)); // ровно догнал — не сброс
    CHECK(!spectrum_base_reset_detected_by_counts(267100, 0, 267049)); // обогнал — рост, не сброс
    CHECK(spectrum_base_reset_detected_by_counts(267048, 0, 267049));  // на 1 меньше — сброс (допуск 0)
    CHECK(!spectrum_base_reset_detected_by_counts(5, 267049, 267054)); // после fold: expected=5 — не сброс
}

// Сценарий целиком (через ОРКЕСТРАЦИЮ spectrum_base_commit — тот же порядок,
// что spectrum.c): D восстановлен -> первый коммит сворачивает базу -> рост
// без сворачивания. Время после fold — забота caller (spectrum.c #FW-12).
// F3 (итоговое ревью 25.09): база=0 до первого коммита (не 600, как раньше) —
// свёртка теперь триггерится ТОЛЬКО по счёту (dev=3 < expected=600-0=600),
// не по времени; со старой базой=600 expected был бы 0 и тест держался бы
// на времени, которое spectrum_base_commit больше не смотрит.
static void test_sequence(void)
{
    uint32_t base_bins[3] = {0, 0, 0};
    uint32_t shown_bins[3] = {100, 200, 300};
    spectrum_base_state_t st = { base_bins, 0, 0, shown_bins, 4451, 600 };

    uint32_t dev_bins[3] = {1, 0, 2};
    CHECK(spectrum_base_commit(&st, dev_bins, 3, 3, true, 11));
    CHECK(st.base_counts == 600 && base_bins[0] == 100 && base_bins[2] == 300);
    st.base_time = st.shown_time = 4451 + 11;
    CHECK(shown_bins[0] == 101 && shown_bins[2] == 302 && st.shown_counts == 603);

    uint32_t dev_bins2[3] = {5, 0, 2};
    CHECK(!spectrum_base_commit(&st, dev_bins2, 7, 3, true, 15));
    CHECK(st.base_counts == 600);
    // dev_total(7) — накопительный СЧЁТ прибора с его последнего сброса (не
    // приращение к прошлому shown), поэтому shown = base(600)+dev(7) = 607.
    CHECK(shown_bins[0] == 105 && st.shown_counts == 607);
}

// Живой сценарий 25.09 (второй прогон): D восстановлен (267049,1634с) ->
// ПЕРВАЯ гистограмма прибора после сброса (маленькая, 9) БЕЗ свежего STAT на
// этом коммите. До фикса merge клеил dev поверх базы 0 ДО проверки сброса —
// база получала 9 вместо 267049. Проверка — именно base_counts == D.
static void test_live_bug_no_stat_first_commit(void)
{
    uint32_t base_bins[3] = {0, 0, 0};
    uint32_t shown_bins[3] = {100000, 100000, 67049};   // сумма 267049, как D
    // база ещё НЕ сворачивала ничего (base_counts=0, base_bins=0) — восстановлен
    // именно ПОКАЗЫВАЕМЫЙ (shown) спектр D, база пуста до первого fold.
    spectrum_base_state_t st = { base_bins, 0, 0, shown_bins, 1634, 267049 };

    uint32_t dev_bins[3] = {3, 2, 4};   // крошечный свежий свип, сумма 9
    bool did = spectrum_base_commit(&st, dev_bins, 9, 3, /*stat_fresh=*/false, 0);

    CHECK(did);
    CHECK(st.base_counts == 267049);   // НЕ 9
    CHECK(base_bins[0] == 100000 && base_bins[2] == 67049);
    CHECK(st.shown_counts == 267049 + 9);
}

// F3 (итоговое ревью 25.09): счёт растёт СОГЛАСОВАННО (dev=610 >= expected
// 600), а STAT-время отстало на 10с (590 < 600-5=595, старый код счёл бы это
// сбросом) — база НЕ должна измениться, счёт НЕ должен удвоиться.
static void test_time_only_regression_no_fold(void)
{
    uint32_t base_bins[3] = {50, 20, 30};       // сумма 100
    uint32_t shown_bins[3] = {350, 200, 150};   // сумма 700 (100 база + 600 dev)
    spectrum_base_state_t st = { base_bins, 1000, 100, shown_bins, 1600, 700 };

    uint32_t dev_bins[3] = {305, 200, 105};     // сумма 610 (>= expected 600)
    bool did = spectrum_base_commit(&st, dev_bins, 610, 3, /*stat_fresh=*/true,
                                     /*dev_time_now=*/590);

    CHECK(!did);
    CHECK(st.base_counts == 100 && base_bins[0] == 50 && base_bins[2] == 30);
    CHECK(st.shown_counts == 100 + 610);   // НЕ 100+610+610 (было бы при баге)
}

// N2 (ревью-2): настоящий сброс, но НОВЫЙ свип успел набрать БОЛЬШЕ старого
// (фон 10ч=36000с/100000 → сброс → горячий источник 300с/200000 > 100000) —
// count-проверка одна видит «выросло», не ловит. Ограниченный сигнал по
// времени (dev_time_now=300 < половины dev_elapsed=36000) обязан поймать.
static void test_n2_count_grew_time_catches(void)
{
    uint32_t base_bins[3] = {0, 0, 0};
    uint32_t shown_bins[3] = {40000, 30000, 30000};  // сумма 100000
    spectrum_base_state_t st = { base_bins, 0, 0, shown_bins, 36000, 100000 };

    uint32_t dev_bins[3] = {80000, 60000, 60000};    // сумма 200000
    bool did = spectrum_base_commit(&st, dev_bins, 200000, 3, /*stat_fresh=*/true,
                                     /*dev_time_now=*/300);

    CHECK(did);
    CHECK(st.base_counts == 100000);
    CHECK(st.base_time == 36000);
    CHECK(base_bins[0] == 40000 && base_bins[1] == 30000);
    CHECK(st.shown_counts == 100000 + 200000);
}

void spectrum_base_plan_suite(void)
{
    test_reset_detection();
    test_merge();
    test_merge_overflow();
    test_counts_reset();
    test_sequence();
    test_live_bug_no_stat_first_commit();
    test_time_only_regression_no_fold();
    test_n2_count_grew_time_catches();
}
