#!/usr/bin/env bash
# Управление Thunderobot R16 (Hunter Blade 16): режим производительности
# и вентиляторы через драйвер gwmi-wmi.
#
#   power-mode.sh {0|1|2}              режим питания
#       0 — максимум:  TGP ~115 Вт + Dynamic Boost (до 140 Вт)
#       1 — сбалансированный (заводской): ~95 Вт
#       2 — энергосбережение: ~80 Вт
#   power-mode.sh status               профиль, GPU, вентиляторы, температуры
#   power-mode.sh fan                  состояние вентиляторов
#   power-mode.sh fan auto             оба вентилятора — по кривой EC (по умолчанию)
#   power-mode.sh fan full             оба на полную скорость
#   power-mode.sh fan <0-100>          оба вручную, в процентах
#   power-mode.sh fan <1|2> <auto|full|0-100>   только вентилятор 1 или 2
#
# Режим выставляется через platform_profile, которым управляет
# power-profiles-daemon (то же самое, что переключатель в трее KDE/GNOME).
# Вентиляторы — через hwmon-устройство gwmi_wmi (pwm1/pwm2).
# Ручной режим: 0% = самая низкая скорость (~780 об/мин), вентилятор не
# останавливается; 100% = максимум (~4800 об/мин).
set -u

usage() {
    sed -n '2,/^set -u/{/^set -u/d;s/^# \{0,1\}//;p}' "$0"
    exit 1
}

need_root() {
    [ "$(id -u)" -eq 0 ] || exec sudo -E bash "$0" "$@"
}

find_hwmon() {
    local h
    for h in /sys/class/hwmon/hwmon*; do
        if [ "$(cat "$h/name" 2>/dev/null)" = "gwmi_wmi" ]; then
            echo "$h"
            return 0
        fi
    done
    return 1
}

show_profile() {
    echo "режим: $(cat /sys/firmware/acpi/platform_profile 2>/dev/null || echo недоступен)"
    if command -v nvidia-smi >/dev/null 2>&1; then
        nvidia-smi -q -d POWER 2>/dev/null \
            | grep -E "Current Power Limit|Default Power Limit" | sed 's/^ *//'
    fi
}

set_profile() {
    local profile
    case "$1" in
        0) profile=performance ;;
        1) profile=balanced ;;
        2) profile=power-saver ;;
    esac
    if command -v powerprofilesctl >/dev/null 2>&1 && powerprofilesctl list >/dev/null 2>&1; then
        powerprofilesctl set "$profile"
    else
        # без power-profiles-daemon у ядра профиль называется low-power
        [ "$profile" = power-saver ] && profile=low-power
        echo "$profile" > /sys/firmware/acpi/platform_profile
    fi
    show_profile
}

show_fans() {
    local h i mode
    h=$(find_hwmon) || { echo "hwmon gwmi_wmi не найден — модуль gwmi_wmi загружен?"; return 1; }
    echo "вентиляторы ($h):"
    for i in 1 2; do
        case "$(cat "$h/pwm${i}_enable" 2>/dev/null)" in
            0) mode="full" ;;
            1) mode="ручной" ;;
            2) mode="auto" ;;
            *) mode="?" ;;
        esac
        printf '  fan%s: %5s об/мин, pwm %3s/255 (~%s%%), режим: %s\n' "$i" \
            "$(cat "$h/fan${i}_input" 2>/dev/null || echo ?)" \
            "$(cat "$h/pwm${i}" 2>/dev/null || echo ?)" \
            "$(( $(cat "$h/pwm${i}" 2>/dev/null || echo 0) * 100 / 255 ))" "$mode"
    done
    for i in 1 2; do
        local t
        t=$(cat "$h/temp${i}_input" 2>/dev/null) || continue
        printf '  %s: %s°C (hotspot)\n' "$(cat "$h/temp${i}_label" 2>/dev/null)" "$((t / 1000))"
    done
}

# fan_apply <канал 1|2> <auto|full|0-100>
fan_apply() {
    local h=$1 ch=$2 val=$3 pwm
    case "$val" in
        auto) echo 2 > "$h/pwm${ch}_enable" ;;
        full) echo 0 > "$h/pwm${ch}_enable" ;;
        *)
            pwm=$(( (val * 255 + 50) / 100 ))
            echo "$pwm" > "$h/pwm${ch}"    # запись pwm сама включает ручной режим
            ;;
    esac
}

valid_fan_value() {
    case "$1" in
        auto|full) return 0 ;;
        ''|*[!0-9]*) return 1 ;;
        *) [ "$1" -le 100 ] ;;
    esac
}

cmd_fan() {
    local h ch val
    if [ $# -eq 0 ]; then
        show_fans
        return
    fi

    case "$1" in
        1|2)
            [ $# -eq 2 ] || usage
            ch=$1; val=$2
            ;;
        *)
            [ $# -eq 1 ] || usage
            ch=all; val=$1
            ;;
    esac
    valid_fan_value "$val" || { echo "ошибка: значение должно быть auto, full или 0-100"; exit 1; }

    h=$(find_hwmon) || { echo "hwmon gwmi_wmi не найден — модуль gwmi_wmi загружен?"; exit 1; }

    if [ "$ch" = all ]; then
        fan_apply "$h" 1 "$val" && fan_apply "$h" 2 "$val"
    else
        fan_apply "$h" "$ch" "$val"
    fi || { echo "ошибка записи в $h"; exit 1; }

    sleep 1
    show_fans
}

CMD=${1:-}
case "$CMD" in
    0|1|2)
        need_root "$@"
        set_profile "$CMD"
        ;;
    status)
        show_profile
        show_fans
        ;;
    fan)
        shift
        # без аргументов — только чтение, root не нужен
        [ $# -eq 0 ] || need_root fan "$@"
        cmd_fan "$@"
        ;;
    *)
        usage
        ;;
esac
