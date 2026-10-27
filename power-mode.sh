#!/usr/bin/env bash
# Управление Thunderobot R16 (Hunter Blade 16): режим производительности,
# вентиляторы и подсветка через драйвер gwmi-wmi.
#
#   power-mode.sh power {0|1|2}        режим питания
#       0 — максимум:  TGP ~115 Вт + Dynamic Boost (до 140 Вт)
#       1 — сбалансированный (заводской): ~95 Вт
#       2 — энергосбережение: ~80 Вт
#   power-mode.sh status               профиль, GPU, вентиляторы, температуры
#   power-mode.sh fan                  состояние вентиляторов
#   power-mode.sh fan auto             оба вентилятора — по кривой EC (по умолчанию)
#   power-mode.sh fan full             оба на полную скорость
#   power-mode.sh fan <0-100>          оба вручную, в процентах
#   power-mode.sh fan <1|2> <auto|full|0-100>   только вентилятор 1 или 2
#   power-mode.sh light                состояние подсветки
#   power-mode.sh light <зона> <цвет>  цвет зоны: left|middle|right|lid|kbd|all
#                                      цвет: RRGGBB или red, green, blue, white,
#                                      orange, yellow, cyan, magenta, violet, off
#   power-mode.sh light <зона> bright <0|1|2>   яркость: 0 — выкл, 2 — максимум
#   power-mode.sh light <зона> mode <режим>     static, off, blink, fade, ...
#
# Режим выставляется через platform_profile, которым управляет
# power-profiles-daemon (то же самое, что переключатель в трее KDE/GNOME).
# Вентиляторы — через hwmon-устройство gwmi_wmi (pwm1/pwm2).
# Ручной режим: 0% = самая низкая скорость (~780 об/мин), вентилятор не
# останавливается; 100% = максимум (~4800 об/мин).
# Подсветка — через LED-устройства gwmi_wmi (зоны клавиатуры и логотип крышки).
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

# show_cpu_power — лимиты пакета CPU из RAPL (PL1 — long_term, PL2 — short_term)
show_cpu_power() {
    local b=/sys/class/powercap/intel-rapl:0 pl1 pl2
    [ -r "$b/constraint_0_power_limit_uw" ] || return 0
    pl1=$(cat "$b/constraint_0_power_limit_uw" 2>/dev/null) || return 0
    pl2=$(cat "$b/constraint_1_power_limit_uw" 2>/dev/null) || pl2=
    case "$pl1" in
        ''|*[!0-9]*) return 0 ;;
    esac
    printf 'CPU: PL1 %s Вт' "$((pl1 / 1000000))"
    case "$pl2" in
        ''|*[!0-9]*) ;;
        *) printf ', PL2 %s Вт' "$((pl2 / 1000000))" ;;
    esac
    echo
}

# show_profile [задержка] — профиль, лимиты CPU и GPU
# (задержка нужна после смены режима: nvidia-smi показывает лимит с запозданием)
show_profile() {
    local wait=${1:-0}
    echo "режим: $(cat /sys/firmware/acpi/platform_profile 2>/dev/null || echo недоступен)"
    if [ "$wait" -gt 0 ]; then
        echo "ждём ${wait} с, пока GPU применит новый лимит..."
        sleep "$wait"
    fi
    show_cpu_power
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
        *) echo "ошибка: режим должен быть 0, 1 или 2"; exit 1 ;;
    esac
    if command -v powerprofilesctl >/dev/null 2>&1 && powerprofilesctl list >/dev/null 2>&1; then
        powerprofilesctl set "$profile"
    else
        # без power-profiles-daemon у ядра профиль называется low-power
        [ "$profile" = power-saver ] && profile=low-power
        echo "$profile" > /sys/firmware/acpi/platform_profile
    fi
    show_profile 5
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

# ---- подсветка ----

LED_ROOT=/sys/class/leds
LED_ZONES="left middle right lid"
LED_MODES="off static blink fade heartbeat wave random rainbow"

# led_dir <зона> — каталог LED-устройства зоны
led_dir() {
    case "$1" in
        lid) echo "$LED_ROOT/gwmi_wmi::lid" ;;
        left|middle|right) echo "$LED_ROOT/gwmi_wmi::kbd_backlight-$1" ;;
        *) return 1 ;;
    esac
}

led_zones_expand() {
    case "$1" in
        left|middle|right|lid) echo "$1" ;;
        kbd) echo "left middle right" ;;
        all) echo "$LED_ZONES" ;;
        *) return 1 ;;
    esac
}

color_code() {
    case "$1" in
        red) echo ff0000 ;;
        green) echo 00ff00 ;;
        blue) echo 0000ff ;;
        white) echo ffffff ;;
        yellow) echo ffff00 ;;
        orange) echo ff8000 ;;
        cyan) echo 00ffff ;;
        magenta|purple) echo ff00ff ;;
        violet) echo 8000ff ;;
        [0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F])
            echo "$1" | tr 'A-F' 'a-f' ;;
        *) return 1 ;;
    esac
}

valid_led_mode() {
    case " $LED_MODES " in
        *" $1 "*) return 0 ;;
    esac
    return 1
}

show_light() {
    local z d found=0
    echo "подсветка:"
    for z in $LED_ZONES; do
        d=$(led_dir "$z")
        [ -e "$d/rgb" ] || continue
        found=1
        printf '  %-6s цвет: %s, яркость: %s/2, режим: %s\n' "$z" \
            "$(cat "$d/rgb" 2>/dev/null)" \
            "$(cat "$d/brightness" 2>/dev/null)" \
            "$(cat "$d/mode" 2>/dev/null)"
    done
    [ "$found" -eq 1 ] || {
        echo "  LED-устройства gwmi_wmi не найдены — модуль gwmi_wmi загружен?"
        return 1
    }
}

# light_color <зона> <RRGGBB|off>
light_color() {
    local d
    d=$(led_dir "$1") || return 1
    if [ "$2" = off ]; then
        echo off > "$d/mode"
    else
        echo static > "$d/mode"   # из анимации обратно в статичный цвет
        echo "$2" > "$d/rgb"
    fi
}

light_brightness() {
    local d
    d=$(led_dir "$1") || return 1
    echo "$2" > "$d/brightness"
}

light_mode() {
    local d
    d=$(led_dir "$1") || return 1
    echo "$2" > "$d/mode"
}

cmd_light() {
    local zs z val kbd_done=0
    if [ $# -eq 0 ]; then
        show_light
        return
    fi

    zs=$(led_zones_expand "$1") || {
        echo "ошибка: зона должна быть left, middle, right, lid, kbd или all"
        exit 1
    }
    [ $# -ge 2 ] || usage

    case "$2" in
        bright)
            [ $# -eq 3 ] || usage
            val=$3
            case "$val" in
                0|1|2) ;;
                *) echo "ошибка: яркость должна быть 0, 1 или 2"; exit 1 ;;
            esac
            # яркость трёх зон клавиатуры общая — пишем её один раз
            for z in $zs; do
                case "$z" in
                    lid)
                        light_brightness lid "$val" || exit 1
                        ;;
                    *)
                        [ "$kbd_done" -eq 1 ] && continue
                        kbd_done=1
                        light_brightness "$z" "$val" || exit 1
                        ;;
                esac
            done
            ;;
        mode)
            [ $# -eq 3 ] || usage
            val=$3
            valid_led_mode "$val" || {
                echo "ошибка: режим должен быть одним из: $LED_MODES"
                exit 1
            }
            for z in $zs; do
                light_mode "$z" "$val" || exit 1
            done
            ;;
        *)
            [ $# -eq 2 ] || usage
            if [ "$2" != off ]; then
                val=$(color_code "$2") || {
                    echo "ошибка: цвет — RRGGBB или red, green, blue, white, orange, yellow,"
                    echo "        cyan, magenta, violet; off — выключить"
                    exit 1
                }
            else
                val=off
            fi
            for z in $zs; do
                light_color "$z" "$val" || exit 1
            done
            ;;
    esac

    show_light
}

CMD=${1:-}
case "$CMD" in
    power)
        shift
        [ $# -eq 1 ] || usage
        case "$1" in
            0|1|2) ;;
            *) usage ;;
        esac
        need_root power "$@"
        set_profile "$1"
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
    light)
        shift
        # без аргументов — только чтение, root не нужен
        [ $# -eq 0 ] || need_root light "$@"
        cmd_light "$@"
        ;;
    *)
        usage
        ;;
esac
