#!/usr/bin/env bash
set -euo pipefail

demo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
pg_config_cmd="${PG_CONFIG:-pg_config}"
pg_bin="$("$pg_config_cmd" --bindir)"
scratch="$(mktemp -d /tmp/streampatch.XXXXXXXX)"
cleanup() {
    if [[ -f "$scratch/data/postmaster.pid" ]]; then
        "$pg_bin/pg_ctl" -D "$scratch/data" -m fast -w stop >/dev/null
    fi
    rm -rf -- "$scratch"
}
trap cleanup EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Wpedantic \
    -I"$("$pg_config_cmd" --includedir)" "$demo_dir/demo.cpp" \
    -L"$("$pg_config_cmd" --libdir)" -lpq -o "$scratch/demo"

"$pg_bin/initdb" -D "$scratch/data" -U streampatch -A trust --no-locale >/dev/null
if ! "$pg_bin/pg_ctl" -D "$scratch/data" -l "$scratch/postgres.log" \
    -o "-c listen_addresses='' -p 5432 -k $scratch" -w start >/dev/null; then
    cat "$scratch/postgres.log" >&2
    exit 1
fi

"$scratch/demo" "host=$scratch port=5432 dbname=postgres user=streampatch" \
    "$demo_dir/a42.events"
