#!/usr/bin/env bash
# #63: старт установки с GitHub не должен выделять память в момент нажатия кнопки (xTaskCreate* внутри ota_gh_install_start()),
# задача создаётся статически один раз в ota_gh_client_init(); нехватка памяти != «уже идёт» (503 против 409).
# Проверка исходника (сборка firmware на хосте невозможна). Мутации (#SA-3): mutate_ota_install_task.sh.
set -u; cd "$(dirname "$0")"
C=${OIT_C:-../../main/ota_github_client.c}; W=${OIT_W:-../../main/web_server.c}; RC=0
fn() { awk -v n="$1" 'index($0,n"(")&&/^[a-z].*\)$/{p=1} p{print} p&&/^}/{exit}' "$C"; }
chk() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; RC=1; fi; }
START=$(fn 'esp_err_t ota_gh_install_start'); INIT=$(fn 'void ota_gh_client_init')
chk start_found           '[ -n "$START" ] && [ -n "$INIT" ]'
chk start_no_task_create  '! grep -q "xTaskCreate" <<<"$START"'
chk start_notifies        'grep -q "xTaskNotifyGive" <<<"$START"'
chk init_static_create    'grep -q "xTaskCreateStaticPinnedToCore(install_worker" <<<"$INIT"'
chk no_dynamic_install    '! grep -qE "xTaskCreate(Pinned.*)?\(install" "$C"'
HB=$(awk '/handle_ota_gh_install\(httpd_req_t/{p=1} p{print} p&&/^}/{exit}' "$W")
L() { grep -n "$1" <<<"$HB" | head -1 | cut -d: -f1; }   # номер строки первого вхождения в теле обработчика
chk nomem_503_before_409  '[ -n "$(L ESP_ERR_NO_MEM)" ] && [ -n "$(L "\"503 ")" ] && [ -n "$(L "\"409 ")" ] && [ "$(L ESP_ERR_NO_MEM)" -le "$(L "\"503 ")" ] && [ "$(L "\"503 ")" -lt "$(L "\"409 ")" ]'
[ $RC -eq 0 ] && echo "ALL PASSED"; exit $RC
