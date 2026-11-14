# Host-тесты shproto — обычный gcc/clang, без ESP-IDF.
# Компонент components/shproto зависит только от stdint/stddef/stdbool/string,
# поэтому конечный автомат протокола тестируется на хосте без железа.
#
#   make test   — собрать и прогнать (ждём "ALL PASSED", код возврата 0)
#   make asan   — то же под AddressSanitizer + UBSan (ловит запись за границы)
#   make clean  — убрать бинарники

CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -I../../components/shproto/include -I../../main
SRCS = test_shproto.c test_roundtrip.c ../../components/shproto/shproto.c \
       test_net_time.c ../../main/net_time.c \
       test_debug_log_filter.c \
       test_hist_stage.c ../../main/spectrum_hist_stage.c \
       test_flash_quiet.c \
       test_kv_array.c \
       test_t1_settle.c ../../main/spectrum_t1.c \
       test_wf_seg_clear.c ../../main/wf_seg_delete.c \
       fuzz_backup_name.c \
       test_acq_watch.c \
       test_acq_intent.c \
       test_backup_plan.c ../../main/backup_plan.c \
       test_restore_plan.c \
       test_wifi_reconnect_plan.c \
       test_wifi_return_plan.c \
       test_spectrum_base_plan.c \
       test_http_activity_plan.c \
       test_boot_flag_plan.c \
       test_ota_image_check.c \
       test_ota_mark_valid_plan.c \
       test_json_uint_fmt.c \
       test_ota_github_version.c \
       test_ota_github_sha256sums.c \
       test_ota_github_parse.c \
       test_ota_busy.c \
       test_ota_github_decision.c \
       test_ota_github_redirect.c \
       test_ota_timeout_budget.c \
       test_ota_github_download_retry.c \
       test_calib_autoread.c \
       test_text_accum.c \
       test_sweepA_small.c \
       test_wf_export_plan.c \
       test_wf_seg_rebuild_range.c \
       test_spec_cache_gen.c \
       test_calib_export.c \
       test_text_accum_r3a.c \
       test_text_accum_r3.c \
       test_round3_misc.c \
       test_wf_ref_plan.c \
       test_session_plan.c \
       test_wf_rst_keep.c \
       test_wf_tail_plan.c \
       test_ota_gh_check_state.c \
       test_debug_log_chunk.c \
       test_spectrum_import_plan.c \
       test_live_time.c \
       test_spectrum_calib_meta.c \
       test_autosave_skip_plan.c \
       test_ttl_plan.c \
       test_spectrum_meta_fmt.c \
       test_spectrum_fold.c \
       test_spectrum_epoch.c \
       test_sweep_missed.c \
       test_task_load.c \
       test_seq_copy.c \
       test_monitor_window.c \
       test_autosave_mark_retry.c \
       test_meta_w_parse.c

test: $(SRCS) test_util.h socket_budget_check.sh ../../main/socket_budget.h test_import_body_plan test_import_tmp_plan test_n42_time_plan test_spectrum_gz_plan test_glue_cache test_glue_monitor test_glue_cache_oom test_glue_meta_w test_glue_fold test_epoch_atomic test_glue_epoch test_spectrum_gz_async_plan test_spectrum_gz_job test_glue_gz_async test_mdns_txt_plan test_tcp_reset test_cmdlog
	$(CC) $(CFLAGS) -o test_runner $(SRCS) -lm
	./test_runner
	bash ./socket_budget_check.sh
	bash ./ota_install_task_check.sh
	./test_import_body_plan
	./test_import_tmp_plan
	./test_n42_time_plan
	./test_spectrum_gz_plan
	./test_glue_cache
	./test_glue_monitor
	./test_glue_cache_oom
	./test_glue_meta_w
	./test_glue_fold
	./test_epoch_atomic
	./test_glue_epoch
	./test_spectrum_gz_async_plan
	./test_spectrum_gz_job
	./test_glue_gz_async
	./test_mdns_txt_plan
	./test_tcp_reset
	./test_cmdlog

# D2 (#SA-3): тот же набор БЕЗ остановки на первом упавшем бинаре - мутационные скрипты D2 (MT_TARGET=test-keepgoing) видят красные тесты ВСЕХ бинарей.
test-keepgoing: $(SRCS) test_util.h socket_budget_check.sh ../../main/socket_budget.h test_import_body_plan test_import_tmp_plan test_n42_time_plan test_glue_cache test_glue_monitor test_glue_cache_oom test_glue_meta_w test_glue_fold test_epoch_atomic test_glue_epoch test_spectrum_gz_async_plan test_spectrum_gz_job test_glue_gz_async test_mdns_txt_plan test_tcp_reset test_cmdlog
	$(CC) $(CFLAGS) -o test_runner $(SRCS) -lm
	@rc=0; for t in ./test_runner "bash ./socket_budget_check.sh" ./test_import_body_plan ./test_import_tmp_plan ./test_n42_time_plan ./test_glue_cache ./test_glue_monitor ./test_glue_cache_oom ./test_glue_meta_w ./test_glue_fold ./test_epoch_atomic ./test_glue_epoch ./test_spectrum_gz_async_plan ./test_spectrum_gz_job ./test_glue_gz_async ./test_mdns_txt_plan ./test_tcp_reset ./test_cmdlog; do $$t || rc=1; done; exit $$rc

# release-gate 1.2.36 pass1 С-2: тесты СКЛЕЙКИ - сам main/*.c (не копия) с заглушками IDF/FreeRTOS из glue/ (учёт блоков glue_alloc).
GLUE_DEPS = glue/glue_stubs.c glue/*.h glue/freertos/*.h
test_glue_cache: test_glue_cache.c glue/glue_cache_env.h $(GLUE_DEPS) ../../main/spectrum_http_cache.c ../../main/spectrum_meta_fmt.h ../../main/spectrum_t1.c
	$(CC) $(CFLAGS) -Iglue -I. -o test_glue_cache test_glue_cache.c glue/glue_stubs.c ../../main/spectrum_t1.c -lm

# З-1 (gate pass1): отказ роста буфера spectrum.json сверх 64 КиБ - без утечки (отдельный бинарь: s_json == NULL на старте)
test_glue_cache_oom: test_glue_cache_oom.c glue/glue_cache_env.h $(GLUE_DEPS) ../../main/spectrum_http_cache.c ../../main/spectrum_meta_fmt.h ../../main/spectrum_t1.c
	$(CC) $(CFLAGS) -Iglue -I. -o test_glue_cache_oom test_glue_cache_oom.c glue/glue_stubs.c ../../main/spectrum_t1.c -lm

# pass2 З-3: сам обработчик meta.json?w= (main/meta_w_handler.h) с самим spectrum_http_cache.c - коды 414/400, байты meta, окно SMA
test_glue_meta_w: test_glue_meta_w.c glue/glue_cache_env.h $(GLUE_DEPS) ../../main/meta_w_handler.h ../../main/meta_w_parse.h ../../main/spectrum_http_cache.c ../../main/spectrum_meta_fmt.h ../../main/spectrum_t1.c
	$(CC) $(CFLAGS) -Iglue -I. -o test_glue_meta_w test_glue_meta_w.c glue/glue_stubs.c ../../main/spectrum_t1.c -lm

test_glue_monitor: test_glue_monitor.c $(GLUE_DEPS) ../../main/monitor.c ../../main/monitor_window.h
	$(CC) $(CFLAGS) -Iglue -I. -o test_glue_monitor test_glue_monitor.c glue/glue_stubs.c -lm

# D1: склейка GET /api/spectrum.json?channels=N с самим spectrum_http_cache.c (коды 400/404, байты v1/v2, чередование)
test_glue_fold: test_glue_fold.c glue/glue_cache_env.h $(GLUE_DEPS) ../../main/spectrum_fold.h ../../main/spectrum_http_cache.c ../../main/spectrum_meta_fmt.h ../../main/spectrum_t1.c
	$(CC) $(CFLAGS) -Iglue -I. -o test_glue_fold test_glue_fold.c glue/glue_stubs.c ../../main/spectrum_t1.c -lm

# D2 (K2): seqlock-снимок с эпохой (детерминированные вклинивания писателя + стресс двумя потоками, pthread)
test_epoch_atomic: test_epoch_atomic.c ../../main/spectrum_epoch.h ../../main/seq_copy.h
	$(CC) $(CFLAGS) -pthread -o test_epoch_atomic test_epoch_atomic.c

# D2 (K2): склейка кэша - эпоха парой со снимком (epoch_cache_gen), ответ без параметра байтово == снимок платы (v1_bytes), обрезка query (400)
test_glue_epoch: test_glue_epoch.c glue/glue_cache_env.h $(GLUE_DEPS) ../../main/spectrum_fold.h ../../main/spectrum_epoch.h ../../main/spectrum_http_cache.c ../../main/spectrum_meta_fmt.h ../../main/spectrum_t1.c
	$(CC) $(CFLAGS) -Iglue -I. -o test_glue_epoch test_glue_epoch.c glue/glue_stubs.c ../../main/spectrum_t1.c -lm

# #AWF-F4: автономный тест со своим main() (import_tmp_plan.h) — какой файл SPEC_DIR чистить на старте.
test_import_tmp_plan: test_import_tmp_plan.c ../../main/import_tmp_plan.h
	$(CC) $(CFLAGS) -o test_import_tmp_plan test_import_tmp_plan.c

# AWF-F2: standalone test (n42_time_plan.h) - N42 StartDateTime in UTC with Z.
test_n42_time_plan: test_n42_time_plan.c ../../main/n42_time_plan.h
	$(CC) $(CFLAGS) -o test_n42_time_plan test_n42_time_plan.c

# #GZ-T (1.2.37-rc90): автономный тест со своим main() (spectrum_gz_plan.h) - уровень, Accept-Encoding, рамка gzip, поколение, запас PSRAM.
test_spectrum_gz_plan: test_spectrum_gz_plan.c ../../main/spectrum_gz_plan.h
	$(CC) $(CFLAGS) -o test_spectrum_gz_plan test_spectrum_gz_plan.c

# Ф4.2 (D3): чистые решения асинхронной отдачи (decide/route/срок/очередь/кольцо) и исполнитель задания (spectrum_gz_job.h, учёт выделений).
test_spectrum_gz_async_plan: test_spectrum_gz_async_plan.c ../../main/spectrum_gz_plan.h
	$(CC) $(CFLAGS) -o test_spectrum_gz_async_plan test_spectrum_gz_async_plan.c

test_spectrum_gz_job: test_spectrum_gz_job.c ../../main/spectrum_gz_job.h ../../main/spectrum_gz_plan.h
	$(CC) $(CFLAGS) -o test_spectrum_gz_job test_spectrum_gz_job.c

# Ф4.2: склейка кэша (main/spectrum_http_cache.c) + исполнитель задания + настоящий zlib: эквивалентность распаковкой, атомарность эпохи, утечки.
test_glue_gz_async: test_glue_gz_async.c glue/glue_cache_env.h $(GLUE_DEPS) ../../main/spectrum_gz_job.h ../../main/spectrum_gz_plan.h ../../main/spectrum_http_cache.c ../../main/spectrum_http_cache.h ../../main/spectrum_fold.h ../../main/spectrum_t1.c
	$(CC) $(CFLAGS) -Iglue -I. -o test_glue_gz_async test_glue_gz_async.c glue/glue_stubs.c ../../main/spectrum_t1.c -lm -lz

# Ф4.2: только три теста асинхронной отдачи, без остановки на первом упавшем (мутационный скрипт mutate_gz_async.sh; строки ALL PASSED / CHECK(S) FAILED для mutlib.sh).
test-gz-keepgoing: test_spectrum_gz_async_plan test_spectrum_gz_job test_glue_gz_async
	@rc=0; for t in ./test_spectrum_gz_async_plan ./test_spectrum_gz_job ./test_glue_gz_async; do $$t || rc=1; done; if [ $$rc -eq 0 ]; then echo "ALL PASSED"; else echo "CHECK(S) FAILED"; fi; exit $$rc

# 1.2.39: автономные тесты со своим main() - TXT службы mDNS _atomspectra._tcp, распознавание -rst без завершающего  в кадре TCP-моста, кольцо журнала команд.
test_mdns_txt_plan: test_mdns_txt_plan.c ../../main/mdns_txt_plan.h
	$(CC) $(CFLAGS) -o test_mdns_txt_plan test_mdns_txt_plan.c

test_tcp_reset: test_tcp_reset.c ../../main/tcp_reset_plan.h ../../main/acq_intent.h
	$(CC) $(CFLAGS) -o test_tcp_reset test_tcp_reset.c

test_cmdlog: test_cmdlog.c ../../main/cmdlog.h
	$(CC) $(CFLAGS) -o test_cmdlog test_cmdlog.c

# Автономный тест со своим main() (import_body_plan.h) — в общий test_runner не линкуется.
test_import_body_plan: test_import_body_plan.c ../../main/import_body_plan.h
	$(CC) $(CFLAGS) -o test_import_body_plan test_import_body_plan.c

# Дополнительный прогон под санитайзерами: даже если canary-проверка пропустит
# регрессию с записью за границу буфера, ASan поймает её жёстко.
asan: $(SRCS) test_util.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o test_runner_asan $(SRCS) -lm
	./test_runner_asan

# 1.2.36 H1 (A): многопоточный стресс seqlock (pthread): писатель под мьютексом с теми же seq++/барьерами,
# что SPEC_LOCK/SPEC_UNLOCK; читатели - seq_copy() без мьютекса; каждый принятый снимок total == сумма bins.
# Отдельно от test_runner: MA1/MA2 (mutate_seq_copy.sh) обязаны краснить его своим прогоном.
stress: test_seq_stress.c ../../main/seq_copy.h
	$(CC) $(CFLAGS) -pthread -o test_seq_stress test_seq_stress.c
	./test_seq_stress

# То же под ThreadSanitizer: гонка копии с писателем ОЖИДАЕМА (риск R-1, копия отбрасывается проверкой seq);
# отчёты не валят прогон (exitcode=0) - их места сводит run-лог.
tsan: test_seq_stress.c ../../main/seq_copy.h
	$(CC) $(CFLAGS) -pthread -fsanitize=thread -g -o test_seq_stress_tsan test_seq_stress.c
	TSAN_OPTIONS="halt_on_error=0 exitcode=0" ./test_seq_stress_tsan

clean:
	rm -f test_runner test_runner_asan bench_json_fmt test_import_body_plan test_import_tmp_plan test_n42_time_plan test_spectrum_gz_plan test_seq_stress test_seq_stress_tsan test_glue_cache test_glue_monitor test_glue_cache_oom test_glue_meta_w test_glue_fold test_epoch_atomic test_glue_epoch test_spectrum_gz_async_plan test_spectrum_gz_job test_glue_gz_async test_mdns_txt_plan test_tcp_reset test_cmdlog

# UI-P1: замер до/после старого(vsnprintf-loop)/нового(json_append_uint32_csv)
# форматтера бинов — числа для отчёта, не гейт ALL PASSED.
bench: bench_json_fmt.c ../../main/json_uint_fmt.h
	$(CC) -std=c11 -Wall -Wextra -O2 -I../../main -o bench_json_fmt bench_json_fmt.c
	./bench_json_fmt

.PHONY: test asan clean bench stress tsan
