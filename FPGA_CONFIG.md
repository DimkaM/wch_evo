# Конфигурация FPGA (Altera, passive serial по SPI1 + DMA)

**Проект:** `D:\src\new_evo\test_usb_1`, ветка `conf_fpga`
**Источник кода:** рабочий проект `D:\src\new_evo\test_altera` (`src/fpga.c`) — последовательность
перенесена без изменений; добавлена только обвязка «после старта» и логи.

---

## 1. Что и когда происходит

Автоматическая конфигурация выполняется **один раз при старте**, после того как поднимется
USB-хост-стек. Последовательностью владеет постоянная задача `power` (`src/app_power.c`):

```
планировщик → usb_host (prio 2): стек USBFS, стоп/старт по PWR_OK (src/app_usb.c)
             → rtos_test (prio 1)
             → power (prio 4): спит DEF_FPGA_CONFIG_DELAY_MS = 3000 мс,
                               POWER_On() → FPGA_Config(),
                               далее обслуживает кнопку PC2 (короткое нажатие — перезаливка,
                               удержание > 3 с — выключение БП), задача НЕ удаляется
```

Задержка нужна, чтобы к моменту конфигурации USB уже работал (флаги готовности
`g_usbRootReady` / `g_usbHidKbReady` печатаются в лог; по клавиатуре за хабом планируется
обработка hot keys).

Повторные заливки выполняются **по кнопке** (см. §9). Автозапуск можно отключить
(`DEF_POWER_AUTO_ON 0` в `src/app_power.h`) — тогда плата после сброса ждёт нажатия кнопки.

## 2. Подключение (как в рабочем проекте)

| Сигнал | Пин | Режим |
|---|---|---|
| nCONFIG | PA2 | выход push-pull |
| nSTATUS | PA3 | вход (floating) |
| CONF_DONE | PA4 | вход (floating) |
| DCLK (SCK) | **PB3** | SPI1, Alternate Function PP (**полный ремап** `GPIO_Remap_SPI1`) |
| DATA0 (MOSI) | **PB5** | SPI1, Alternate Function PP |
| GND | — | общий |

Отладка идёт по SWD (PA13/PA14), поэтому JTAG-пины PB3/PB4/PA15 не мешают; USBFS
(PA11/PA12) и USART1 (PA9) не затрагиваются.

## 3. Параметры (`src/fpga.h`)

| Макрос | Значение | Смысл |
|---|---|---|
| `DEF_FPGA_CONFIG_EN` | 1 | включить/отключить заливку |
| `DEF_FPGA_CONFIG_DELAY_MS` | 3000 | задержка от старта до конфигурации (реальные мс) |
| `DEF_FPGA_CONFIG_PRIO` | 4 | приоритет задачи (выше USB-задачи = 2) |
| `DEF_FPGA_CONFIG_STACK_WORDS` | 256 | стек задачи (слов) |
| `DEF_FPGA_CONFIG_BLOCK_SIZE` | 512 | байт на один блок DMA (можно увеличить до 65535) |
| `DEF_FPGA_CONFIG_NSTATUS_MS` | 1000 | таймаут ожидания nSTATUS после сброса |
| `DEF_FPGA_CONFIG_CONFDONE_MS` | 100 | таймаут ожидания CONF_DONE после передачи |

SPI1: master, 8 бит, CPOL=0 / CPHA=1, NSS программный, **первым идёт младший бит**
(`SPI_FirstBit_LSB`) — как требует RBF; prescaler 16 → при SystemCoreClock 96 МГц DCLK = 6 МГц.

## 4. Последовательность (Altera passive serial)

1. `nCONFIG = 0`, пауза 2 мс, `nCONFIG = 1` (сброс ПЛИС).
2. Ожидание `nSTATUS = 1` (таймаут `DEF_FPGA_CONFIG_NSTATUS_MS`).
3. Передача RBF блоками по 512 Б через `DMA1_Channel3` → SPI1; после каждого блока
   проверяется `nSTATUS` (низкий = ошибка приёма).
4. Ожидание сброса `SPI_I2S_FLAG_BSY` (последние биты вытолкнуты из сдвигового регистра).
5. Ожидание `CONF_DONE = 1` (таймаут `DEF_FPGA_CONFIG_CONFDONE_MS`).
6. Финализация: `SPI_Cmd(DISABLE)`, PB3/PB5 переводятся в обычные GPIO-выходы, DATA0 = 1,
   программно выдаются 20 тактов DCLK.

## 5. Битстрим

Данные лежат в `src/rbf.h` (копия из рабочего проекта):

```c
const uint8_t fpga_rbf_data[98023] = { ... };   /* 98 023 байта, ~50 % flash */
```

Массив `const` → размещается во flash и читается DMA напрямую (ОЗУ не тратится).
**Обновление:** сгенерировать `.rbf` в Quartus и заменить `src/rbf.h` (можно тем же
`xxd -i`/скриптом, который использовался для `test_altera`); менять нужно только содержимое
массива, размер подхватывается из `sizeof`.

## 6. Логи (COM4, 115200)

```
PWR: startup in 3000 ms
USB: stack stopped (PSU off)        <- при DEF_USB_OFF_WHEN_PSU_OFF
PWR: POWER_ON asserted (attempt 1)
PWR: POWER_GOOD=1 (N ms)            <- N <= 500
USB: stack started (PSU on)         <- стек поднят, идёт перечисление
FPGA: start (usbRoot=1 usbHidKb=1)
FPGA: nSTATUS OK, sending 98023 B
FPGA: sent 98023 B in NNN ms
FPGA: CONF_DONE=1, FPGA configured
BTN: armed
BTN: short press (N ms) -> FPGA reconfiguration
BTN: long press (N ms) -> PSU off
```

Ошибки и диагностика:

* `FPGA: nSTATUS timeout (N ms)`, `FPGA: nSTATUS lost after N B`, `FPGA: CONF_DONE timeout`,
  `FPGA: configuration (<причина>) FAILED (attempt N of M)`;
* `PWR: PWR_OK did not fall within N ms` — `PC0 = 0` не выключает БП (проверить ключ NPN:
  напряжение базы должно быть ≈0 В, `PS_ON#` ≈5 В);
* `FPGA: idle levels nSTATUS=0 CONF_DONE=0` — на выводах ПЛИС нет «готовности»: устройство не
  питается либо `nSTATUS` не подключён (у запитанной ПЛИС `nSTATUS` в покое = 1 за счёт
  внутренней подтяжки);
* `FPGA: PSU is on, doing a clean power cycle` + `PWR: rails discharged (PWR_OK=0)` — сброс
  предыдущего состояния питания перед повторным включением.

## 7. Режимы проекта

* **FreeRTOS** (`DEF_FREERTOS_EN 1`) — задача `power` (`src/app_power.c`, prio 4, 384 слова),
  описана в §1; она же обслуживает кнопку.
* **Bare-metal** (`DEF_FREERTOS_EN 0`) — в суперцикле `main()`: `AppUsb_Step()` (стоп/старт стека
  по `PWR_OK`), через `DEF_FPGA_CONFIG_DELAY_MS` однократно `AppPower_Startup()` (включение БП +
  заливка), далее `AppPower_Step()` — обслуживание кнопки.
  Триггер по задержке, а не по `g_usbRootReady`: при `DEF_USB_OFF_WHEN_PSU_OFF = 1` стек заглушен,
  пока БП выключен, поэтому флаг готовности не выставился бы никогда.

## 8. Ресурсы и ограничения

* Flash: ≈ **131 КБ** из 256 КБ (51.4 %, из них битстрим 98 КБ), RAM ≈ 27 % (17.8 КБ из 64 КБ).
* Куча FreeRTOS: задача `power` — 384 слова (1.5 КБ) вместо 256 у прежней `fpga_cfg`; следить за
  `heapMin` в логе.
* Между блоками DMA возможны короткие паузы DCLK (окна `vTaskSuspendAll()` USB-задачи);
  Altera PS это допускает. Если понадобится — увеличить `DEF_FPGA_CONFIG_BLOCK_SIZE`.
* Если первая попытка заливки не удалась (например, `nSTATUS timeout`, пока местные стабилизаторы
  платы ПЛИС ещё выходят на режим), она повторяется (`DEF_FPGA_CONFIG_RETRY = 1`, пауза
  `DEF_FPGA_CONFIG_RETRY_MS = 500` в `src/fpga.h`).
* Hot keys (по клавиатуре за хабом) — точка расширения: флаги готовности публикуются
  (`src/USB_Host/app_km.h`), логику проверки клавиш добавлять в `AppPower_Step()`
  (`src/app_power.c`), где уже обслуживается кнопка.

## 9. Перезаливка по кнопке и политика USB

* Кнопка — PC2 (замыкание на GND, подтяжка к VDD), антидребезг 20 мс; поведение, параметры и
  лог-строки описаны в `PINS.md` §10.
* Короткое нажатие при включённом БП → `FPGA_Config()` заново. Повторные заливки безопасны:
  `FPGA_Peripheral_Init()` выполняется в начале каждой заливки, а `FPGA_DMA_StartBlock()` очищает
  `DMA1_FLAG_TC3` перед каждым блоком (`src/fpga.c`), поэтому «залипший» флаг от предыдущей
  передачи не пропускает ожидание.
* Перед заливкой: `POWER_On()`; если БП уже был включён (например, после сброса МК), сначала
  выполняется чистый power-cycle — `PS_ON#` отпускается, ожидается падение `PWR_OK`
  (`DEF_POWER_OFF_CONFIRM_MS = 2000`), затем блок включается снова. После `PWR_OK` выдерживается
  `DEF_POWER_SETTLE_MS = 300`, чтобы местные стабилизаторы платы ПЛИС и её POR успели отработать.
* USB: дефайны `DEF_USB_OFF_WHEN_PSU_OFF`, `DEF_USB_RESTART_ON_POWER_ON`,
  `DEF_USB_RESTART_ON_FPGA_CONFIG`, `DEF_USB_RESTART_DELAY_MS` в `src/USB_Host/usb_host_config.h`;
  реализация — `src/app_usb.c`. По умолчанию: стек глушится, пока `PWR_OK == 0`, и перезапускается
  при включении БП; перезапуск после заливки ПЛИС выключен (`0`), включается одним символом, если
  окажется, что конфигурация ПЛИС мешает USB-части.
