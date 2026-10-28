#!/bin/sh
# -----------------------------------------------------------------------------
# Перевірка правил залежностей між шарами прошивки (вимога 4.5 ЛР2).
# Запуск з кореня репозиторію: make check-deps (у Docker) або sh scripts/check_deps.sh
#
# Дозволений напрямок: App -> Platform (лише platform.h) -> Devices -> нічого.
#   Devices/   бачить лише dev_bus.h і стандартну бібліотеку
#   App/       не бачить HAL, CubeMX і внутрішні файли Platform (board.h, periph_check.h)
#   Platform/  не залежить від App/
#   platform.h — публічне обличчя Platform для App/, теж без HAL
# -----------------------------------------------------------------------------

FW=firmware
HAL_CUBE='stm32[a-z0-9_]*\.h|main\.h|gpio\.h|i2c\.h|spi\.h|tim\.h|usart\.h'
PLATFORM_INTERNAL='board\.h|periph_check\.h'
PLATFORM_ALL="platform\.h|$PLATFORM_INTERNAL"
APP='app\.h'

status=0

# check <опис> <файли> <заборонені заголовки, regex>
check()
{
  desc=$1
  files=$2
  forbidden=$3
  [ -n "$files" ] || return 0
  hits=$(grep -nE "^[[:space:]]*#[[:space:]]*include[[:space:]]*[<\"]($forbidden)[>\"]" $files)
  if [ -n "$hits" ]; then
    echo "check-deps: FAIL — $desc"
    echo "$hits" | sed 's/^/    /'
    status=1
  else
    echo "check-deps: OK   — $desc"
  fi
}

check "Devices/ не включає HAL, CubeMX, Platform, App" \
      "$(ls $FW/Devices/*.[ch] 2>/dev/null)" "$HAL_CUBE|$PLATFORM_ALL|$APP"

check "App/ не включає HAL, CubeMX і внутрішні файли Platform" \
      "$(ls $FW/App/*.[ch] 2>/dev/null)" "$HAL_CUBE|$PLATFORM_INTERNAL"

check "Platform/ не залежить від App/" \
      "$(ls $FW/Platform/*.[ch] 2>/dev/null)" "$APP"

check "platform.h (інтерфейс для App/) без HAL і CubeMX" \
      "$FW/Platform/platform.h" "$HAL_CUBE|$PLATFORM_INTERNAL"

exit $status
