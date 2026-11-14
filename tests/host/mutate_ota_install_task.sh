#!/usr/bin/env bash
# #63 (#SA-3): мутанты на КОПИЯХ main/ota_github_client.c и main/web_server.c (исходники не трогаем).
# baseline и ИСХОДНЫЙ код до правки (git show 4f369b2) -- красные ровно там, где нужно; baseline -- зелёный.
set -u; cd "$(dirname "$0")"; T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; RC=0
C=../../main/ota_github_client.c; W=../../main/web_server.c
run() {   # run <имя> <ждём: pass|fail> <C-файл> <W-файл>
    OIT_C="$3" OIT_W="$4" bash ota_install_task_check.sh >"$T/out" 2>&1; rc=$?
    got=pass; [ $rc -ne 0 ] && got=fail
    echo "== $1: $got (ждали $2) красных: $(grep -c '^FAIL' "$T/out")"; [ "$got" = "$2" ] || RC=1
}
run baseline pass "$C" "$W"
git show 4f369b2:main/ota_github_client.c >"$T/old_c.c" 2>/dev/null && git show 4f369b2:main/web_server.c >"$T/old_w.c"
run original_4f369b2 fail "$T/old_c.c" "$T/old_w.c"
sed 's/xTaskNotifyGive(s_install_worker);/xTaskCreatePinnedToCore(install_task, "x", 8192, NULL, 4, NULL, 1);/' "$C" >"$T/m1.c"
run M1_create_in_start fail "$T/m1.c" "$W"
sed 's/xTaskCreateStaticPinnedToCore(install_worker/xTaskCreatePinnedToCore(install_worker/' "$C" >"$T/m2.c"
run M2_init_dynamic fail "$T/m2.c" "$W"
sed 's/503 Service Unavailable/409 Conflict/' "$W" >"$T/m3.c"
run M3_nomem_as_409 fail "$C" "$T/m3.c"
[ $RC -eq 0 ] && echo "ALL PASSED"; exit $RC
