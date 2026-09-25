# Базовий образ - стабільна Ubuntu 22.04 LTS
FROM ubuntu:22.04

# Вимикаємо інтерактивні запитання під час інсталяції пакетів
ENV DEBIAN_FRONTEND=noninteractive

# Встановлюємо крос-компілятор для ARM та утиліти збірки
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    make \
    gcc-arm-none-eabi \
    binutils-arm-none-eabi \
    libnewlib-arm-none-eabi \
    git \
    && rm -rf /var/lib/apt/lists/*

# Робоча директорія всередині контейнера
WORKDIR /workspace

# За замовчуванням при запуску виконуємо make
CMD ["make"]