#!/usr/bin/env bash
# Тест локальной сборки bitland-mifs-wmi с вашими GUID.
# Каждый GUID проверяется отдельной загрузкой модуля с force_guid=<GUID>.
set -u

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -E bash "$0" "$@"
fi

HERE=$(cd "$(dirname "$0")" && pwd)
KO="$HERE/bitland-probe/bitland-mifs-wmi.ko"
MOD=bitland-mifs-wmi

GUIDS=(
    1F13AB7F-6220-4210-8F8E-8BB5E71EE969
    2BC49DEF-7B15-4F05-8BB7-EE37B9547C0B
    A6FEA33E-DABF-46F5-BFC8-460D961BEC9F
    644C5791-B7B0-4123-A90B-E93876E0DAAD
    101F83BB-30B7-DF11-8D81-0800200C9A66
)

[ -f "$KO" ] || { echo "нет $KO — сначала соберите модуль"; exit 1; }

modprobe -r "$MOD" 2>/dev/null || true
modprobe sparse_keymap 2>/dev/null || true

for g in "${GUIDS[@]}"; do
    echo "== $g =="
    before=$(dmesg | wc -l)

    if ! insmod "$KO" force_guid="$g" 2>/tmp/opencode/insmod.err; then
        echo "   insmod: ОШИБКА: $(tr -d '\n' < /tmp/opencode/insmod.err)"
        tail -n 5 /tmp/opencode/insmod.err
        exit 1
    fi
    sleep 1

    dmesg | tail -n +$((before + 1)) | grep -iE "bitland|mifs" | sed 's/^/   /' || true
    echo "   привязано: $(ls -l /sys/bus/wmi/devices/*/driver 2>/dev/null | grep -c bitland-mifs-wmi) устройств(о)"

    if [ -s /sys/firmware/acpi/platform_profile_choices ]; then
        echo
        echo "   УСПЕХ: $g"
        echo "   choices: $(tr '\n' ' ' < /sys/firmware/acpi/platform_profile_choices)"
        echo "   профиль: $(cat /sys/firmware/acpi/platform_profile 2>/dev/null)"
        for h in /sys/class/hwmon/hwmon*; do
            [ "$(cat "$h/name" 2>/dev/null)" = "bitland_mifs" ] || continue
            echo "   hwmon $h:"
            for a in temp1_input fan1_input fan2_input fan3_input; do
                [ -e "$h/$a" ] && printf '     %-12s %s\n' "$a" "$(cat "$h/$a")"
            done
        done
        echo
        echo "   Дальше (модуль оставлен загруженным):"
        echo "     echo performance | sudo tee /sys/firmware/acpi/platform_profile"
        echo "     nvidia-smi -q -d POWER | grep -A4 'GPU Ceiling'"
        exit 0
    fi

    rmmod "$MOD" 2>/dev/null || true
    echo
done

echo "== ни один GUID не дал platform_profile =="
echo "Смотрите dmesg: 'probe failed' с -EPROTO/-EIO = протокол не совпал."
