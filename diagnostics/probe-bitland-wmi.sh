#!/usr/bin/env bash
# Проба bitland-mifs-wmi на GUID-варианты Thunderobot R16 через прямой bind.
# driver_override на шине WMI не работает, поэтому биндим вручную.
set -u

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -E bash "$0" "$@"
fi

MOD=bitland-mifs-wmi
DRV=/sys/bus/wmi/drivers/$MOD
ERR=/tmp/opencode/bitland-bind.err
mkdir -p /tmp/opencode

GUIDS=(
    1F13AB7F-6220-4210-8F8E-8BB5E71EE969
    2BC49DEF-7B15-4F05-8BB7-EE37B9547C0B
    A6FEA33E-DABF-46F5-BFC8-460D961BEC9F
    644C5791-B7B0-4123-A90B-E93876E0DAAD
    101F83BB-30B7-DF11-8D81-0800200C9A66
)

modprobe "$MOD" 2>/dev/null || { echo "не удалось загрузить $MOD"; exit 1; }

echo "== исходно =="
lsmod | grep bitland_mifs_wmi
echo

for g in "${GUIDS[@]}"; do
    dev="/sys/bus/wmi/devices/$g"
    echo "== $g =="
    if [ ! -d "$dev" ]; then
        echo "   устройства нет, пропуск"
        continue
    fi

    printf '' > "$dev/driver_override" 2>/dev/null || true
    before=$(dmesg | wc -l)

    if printf '%s' "$g" > "$DRV/bind" 2>"$ERR"; then
        echo "   bind: OK — probe прошёл"
    else
        echo "   bind: отказ ($(tr -d '\n' < "$ERR" 2>/dev/null))"
    fi
    sleep 1

    echo "   refs: $(lsmod | awk '$1 == "bitland_mifs_wmi" { print $3 }')"
    new=$(dmesg | tail -n +$((before + 1)) | grep -iE "bitland|mifs" || true)
    [ -n "$new" ] && echo "$new" | sed 's/^/   /'

    if [ -s /sys/firmware/acpi/platform_profile_choices ]; then
        echo
        echo "   ПОДОШЁЛ: $g"
        echo "   choices: $(tr '\n' ' ' < /sys/firmware/acpi/platform_profile_choices)"
        echo "   текущий: $(cat /sys/firmware/acpi/platform_profile 2>/dev/null)"
        echo "   hwmon:"
        for h in /sys/class/hwmon/hwmon*; do
            [ "$(cat "$h/name" 2>/dev/null)" = "bitland_mifs" ] || continue
            echo "     $h"
            for a in temp1_input fan1_input fan2_input fan3_input; do
                [ -e "$h/$a" ] && printf '       %-12s %s\n' "$a" "$(cat "$h/$a")"
            done
        done
        echo
        echo "== ДАЛЬШЕ =="
        echo "  echo performance | sudo tee /sys/firmware/acpi/platform_profile"
        echo "  nvidia-smi -q -d POWER | grep -A4 'GPU Ceiling'"
        echo
        echo "== ОТКАТ =="
        echo "  echo balanced | sudo tee /sys/firmware/acpi/platform_profile"
        echo "  printf '%s' $g | sudo tee $DRV/unbind"
        echo "  sudo modprobe -r $MOD"
        exit 0
    fi

    printf '%s' "$g" > "$DRV/unbind" 2>/dev/null || true
    echo
done

echo "== ни один GUID не дал platform_profile =="
echo "Проверьте dmesg: если probe падал с ошибкой метода — это другой вариант MOF."
modprobe -r "$MOD" 2>/dev/null || true
