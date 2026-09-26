# SPI-интерфейс к ПЛИС: ZX-порты и wait-порты

**Проект:** `D:\src\new_evo\test_usb_1`, ветка `zx_spi` (базируется на `usb_hub_fix`)
**Первоисточники:** AVR-проект `D:\src\pentevo\avr\baseconf\trunk\src` (`spi.c`, `zx.c`, `zx.h`,
`main.h`, `rtc.c/.h`, `interrupts.c`, `main.c`) и HDL ПЛИС
`D:\src\pentevo\fpga\baseconf\trunk` (`slave/slavespi.v`, `zports.v`, `zwait.v`, `top.v`,
`quartus/top.qsf`).
**Состояние:** реализован шаг 1 — транспорт (`spi_init( )` / `spi_send( )`), регистровые обмены
(`zx_spi_send( )`), сервис wait-портов по прерыванию (`zx_wait_task( )`, задача `zx`), Gluk-слой
поверх нашего DS12887 (`gluk_*` в `src/rtc.c`). Клавиатурно-мышиный слой и измерение потолка
скорости — следующие шаги.

---

## 1. Раскладка (подтверждена по плате)

| Сигнал ПЛИС (EP1K50) | Пин ПЛИС | МК | Режим МК | В коде (`src/spi.h`) |
|---|---|---|---|---|
| `spick` | 184 (`GLOBAL_SIGNAL`, `CLOCK_SETTINGS "SPI clock"`) | **PB3** | AF PP (SPI1 полный ремап) | `SPI_SCK_PIN` |
| `spido` | 157 | **PB5** | AF PP | `SPI_MOSI_PIN` |
| `spidi` | 158 | **PB4** | вход (floating) | `SPI_MISO_PIN` |
| `spics_n` | 182 (`GLOBAL_SIGNAL`) | **PB6** | выход PP, в покое `1` | `nSPICS`, `nSPICS_PORT` |
| `spiint_n` | 159 | **PB7** | вход **IPU** + EXTI7 (фронт «падающий») | `SPI_IRQ_PIN`, `SPI_IRQ_EXTI_LINE` |

* PB3/PB5 — **те же** выводы, что `DCLK`/`DATA0` конфигурации: после конфигурации ПЛИС принимает
  их как пользовательские `spick`/`spido` (в ZX-Evo AVR SCK/MOSI разведены на оба набора
  выводов, поэтому перемычек не требуется).
* PB4 — это `JNTRST` (JTAG). Ремап SPI1 JTAG не выключает (см. `PINS.md` §2), но на железе так
  уже работает конфигурация (PB3 как выход SCK); `GPIO_Remap_SWJ_Disable` использовать нельзя —
  он гасит и SWD.
* `spiint_n` притянут внутри МК (`IPU`): до конфигурации ПЛИС её выводы в Z, линия читается как 1,
  ложных прерываний нет.

---

## 2. Протокол

SPI: **mode 0 (CPOL = 0, CPHA = 0), LSB-first, мастер — МК**, частота `96 / 16 = 6 МГц`
(в AVR было `Fosc/2 = 5.53 МГц` при `Fosc = 11.0592 МГц`). Одна транзакция = импульс + байт
«адрес» + байт «данные» (полный аналог AVR `zx_spi_send(addr, data, mask)`):

```
nSPICS:  ‾‾|__|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|______|‾‾‾‾
            импульс     байт «АДРЕС»  байт «ДАННЫЕ»
MISO:               = СТАТУС          = текущее значение регистра
MOSI:               = № регистра      = новое значение
```

Как это работает в ПЛИС (`slave/slavespi.v`):

* `shift_out <= scs_n ? status_in : data_in` на **любом** фронте `spics_n` — поэтому байт при
  `nSPICS = 1` (фаза адреса) отдаёт **статус**, а байт при `nSPICS = 0` (фаза данных) — значение
  выбранного регистра;
* адрес (`regnum`) сдвигается по фронтам SCK **только при `scs_n = 1`** и сбрасывается по фронту
  `scs_n` вверх (`scs_n_01`), т.е. транзакции независимы;
* **действия выполняются по фронту `nSPICS` вверх** (`scs_n_01`): запись регистра, стробы
  клавиатуры/мыши/джойстика, `genrst` (сброс Z80, регистр 0x30), `wait_end` (регистр 0x40);
* чтение = «отправить адрес, затем фиктивные данные (0xFF) — принятое значение и есть содержимое
  регистра»; для некоторых адресов достаточно только фазы адреса (например `SPI_KBD_STB`).

### 2.1 Карта регистров (числа из `zx.h` AVR и `slavespi.v`)

| № | Регистр | Чтение (MISO фазы данных) | Запись (MOSI / действие по фронту CS) |
|---|---|---|---|
| 0x10 | `SPI_KBD_DAT` | — | 40-битный `kbd_reg` (5 байт, LSB-first) |
| 0x11 | `SPI_KBD_STB` | — | строб клавиатуры в порт Z80 |
| 0x20 | `SPI_MOUSE_X` | — | `common_reg` + строб `mus_xstb` |
| 0x21 | `SPI_MOUSE_Y` | — | строб `mus_ystb` |
| 0x22 | `SPI_MOUSE_BTN` | — | строб `mus_btnstb` |
| 0x23 | `SPI_KEMPSTON_JOYSTICK` | — | строб `kj_stb` |
| 0x30 | `SPI_RST_REG` | — | `genrst` — положительный импульс сброса Z80 |
| **0x40** | **`SPI_WAIT_DATA`** | **байт, который записал Z80** | **байт для чтения Z80 + завершение wait** |
| **0x41** | **`SPI_GLUK_ADDR`** | **адрес, выставленный Z80** | (dummy 0xFF) |
| 0x42 | `SPI_RS232_ADDR` | адрес порта RS232 | — |
| 0x50 | `SPI_CONFIG_REG` | — | config0 (видео/лента/NMI) |
| 0x51 | `SPI_CONFIG1_REG` | — | config1 |
| 0x60 | `SPI_SDDATA_REG` | `sd_dataout` | данные SD (spihub) |
| 0x61 | `SPI_SDCTRL_REG` | `{sd_lock_in, xxxxxxx}` | `{lock, cs_n}` |

Для адресов, которых нет в списке, MISO не определён (в AVR принято писать `0xFF` и не смотреть
ответ).

### 2.2 Статус (MISO фазы адреса)

`status_in = { wr_n, waits[6:0] }` (`top.v: .status_in({/* wait_rnw */ wr_n, waits[6:0]})`):

* **bit 7**: `1` — Z80 **читал** wait-порт (`wr_n = 1`), `0` — **писал**;
* **bits 6..0**: индекс wait-порта: `ZXW_GLUK_CLOCK = 0x01`, `ZXW_KONDR_RS232 = 0x02`.

Ноль в bits 6..0 означает «нет ожидающего порта» (в `zx_wait_task( )` такой статус считается
ложным прерыванием и ничего в FPGA не пишется).

### 2.3 Механика wait (ПЛИС удерживает Z80)

1. Z80 обращается к порту Gluk-clock → ПЛИС поднимает `wait_n` (Z80 остановлен) и прижимает
   `spiint_n` (прерывание к МК, в AVR это INT6).
2. МК читает статус → узнаёт порт и направление (`wr_n`).
3. **Чтение** (`status & 0x80`): `addr = zx_spi_send(0x41, 0xFF, 0)` → `data = gluk_get_reg(addr)`
   → `zx_spi_send(0x40, data, 0)`; по фронту CS ПЛИС выставляет `wait_end`, кладёт `wait_reg` на
   шину Z80 и снимает `wait_n`.
4. **Запись**: `addr = zx_spi_send(0x41, 0xFF, 0)`, `data = zx_spi_send(0x40, 0xFF, 0)` (это и
   есть байт Z80) → `gluk_set_reg(addr, data)`.
5. `flags_register &= ~FLAG_SPI_INT` выполняется в начале `zx_wait_task( )`.

---

## 3. Реализация в проекте

| Файл | Что содержит |
|---|---|
| `src/spi.c` / `src/spi.h` | `spi_init( )`, `spi_deinit( )`, `spi_send( )`, `spi_ready( )`, `spi_lock_init/spi_lock/spi_unlock( )`; пины `nSPICS`/`SPI_*`, переключатели `DEF_ZX_SPI_EN`, `DEF_ZX_SPI_PRESCALER` (по умолчанию `/16` = 6 МГц), `DEF_ZX_SPI_DEBUG` |
| `src/zx.c` / `src/zx.h` | числа регистров `SPI_*`, wait-порты `ZXW_*`, `flags_register`/`FLAG_SPI_INT`, `zx_init( )`, `zx_spi_send( )`, `zx_wait_task( )`, `zx_service( )`, `zx_task_start( )`, ISR `EXTI9_5_IRQHandler` и задача `vZxTask`; `DEF_ZX_GLUK_EN`, `DEF_ZX_GLUK_EVO_EXT`, `DEF_ZX_TASK_PRIO`, `DEF_ZX_TASK_STACK_WORDS` |
| `src/rtc.c` / `src/rtc.h` | слой Gluk-часов: `gluk_regs[14]`, `gluk_init( )`, `gluk_inc( )`, `gluk_get_reg( )`, `gluk_set_reg( )` поверх DS12887-эмуляции; имена `GLUK_REG_*`, `GLUK_B_*`, `GLUK_C_*`, `GLUK_*_INIT_VALUE` |
| `src/fpga.c` | `FPGA_Config( )` = взять шину (`spi_lock`) → `spi_deinit( )` → `FPGA_ConfigMain( )` → отпустить шину → при успехе `zx_init( )` |
| `src/app_tasks.c` | создание задачи `zx` (приоритет `DEF_ZX_TASK_PRIO = 5`, стек 192 слова) и мьютекса шины |
| `src/main.c` | bare-metal: `zx_service( )` в суперцикле |

### 3.1 Соответствие имён AVR-проекту

| AVR | Здесь |
|---|---|
| `spi_init( )`, `spi_send(byte)` | `src/spi.c` — те же имена и семантика; внутри SPI1 (ремап, mode 0, LSB) |
| `nSPICS`, `nSPICS_PORT`, `nSPICS_PIN` | `src/spi.h` (`GPIO_Pin_6` / `GPIOB`); вместо `nSPICS_DDR` — `Spi_GpioInit( )` |
| `zx_spi_send(addr,data,mask)`, `zx_init( )`, `zx_wait_task(status)` | `src/zx.c` — те же имена и порядок операций |
| `SPI_*`, `ZXW_*` | `src/zx.h` (плюс `SPI_CONFIG1_REG`/`SPI_SDDATA_REG`/`SPI_SDCTRL_REG`, которых в AVR-заголовке не было) |
| `flags_register`, `FLAG_SPI_INT` | `src/zx.h` / `src/zx.c` (в AVR лежало в `main.h`) |
| `ISR(INT6_vect) { flags_register \|= FLAG_SPI_INT; }` | `EXTI9_5_IRQHandler( )` (PB7) плюс нотификация задачи |
| блок «event from SPI» из `main( )` | `zx_service( )` — тело один-в-один, вызывается из задачи `zx` (RTOS) или суперцикла |
| `gluk_init/gluk_inc/gluk_get_reg/gluk_set_reg`, `gluk_regs[14]`, `GLUK_*` | `src/rtc.c` / `src/rtc.h` |
| `zx_task( )`, `zx_mouse_task( )`, `shift_pause` | пока не портированы (клавиатурно-мышиный слой, следующий шаг) |

### 3.2 Отличия от AVR (осознанные)

1. **Общий SPI1.** У AVR SPI занят только этим интерфейсом; у нас те же SCK/MOSI несёт
   конфигурация ПЛИС, поэтому шина защищена мьютексом (`spi_lock`), а `FPGA_Config( )` держит её
   на всё время передачи (транзакция в середине битстрима испортила бы конфигурацию).
2. **Сервис в задаче.** AVR обслуживает запрос в главном цикле; здесь прерывание только ставит
   `FLAG_SPI_INT` и будит задачу `zx` (приоритет выше USB и power), поэтому Z80 стоит в wait
   меньше. Таймаут ожидания уведомления — 1 мс: пропущенное уведомление не подвесит Z80.
3. **`gluk_inc( )` — пустая.** В AVR она тикала секунды от линии ПЛИС; у нас время ведёт
   RTC-счётчик МК, поэтому секунды идут сами (функция сохранена ради совпадения вызовов).
4. **BCD/HEX.** В AVR `gluk_regs[ ]` хранит «бинарные» значения и конвертирует их при обмене с
   микросхемой; наш DS12887-слой уже отдаёт и принимает байты так, как их видит настоящая
   микросхема (BCD или binary по биту `B.DM`), поэтому `gluk_get_reg/set_reg` — тонкие обёртки.
5. **Расширения ZX-Evolution** (регистр A = адрес EEPROM, регистр C — флаги LED/лога/EEPROM,
   регистр D — состояние клавиш, адреса ≥ 0xF0, ячейки 0xFD..0xFF) **не обслуживаются**:
   `gluk_set_reg( )` для индекса > 0x3F печатает предупреждение (не более 8 раз за прогон).
   Включаются позже вместе с клавиатурным слоем (`DEF_ZX_GLUK_EVO_EXT`).

6. **Задача `zx` обязана блокироваться.** Она имеет приоритет выше power/USB, поэтому любое
   «кручение» в её цикле морит всю систему: на практике `pdMS_TO_TICKS( 1 )` при
   `configTICK_RATE_HZ = 500` равно **0 тиков**, `ulTaskNotifyTake(..., 0)` не блокирует, и МК
   замирал сразу после `[RTOS] zx task: created` (БП вообще не включался, потому что power-задача
   не успевала вызвать `POWER_On( )`). Исправлено: таймаут ожидания уведомления —
   `DEF_ZX_TASK_POLL_TICKS` (1 тик = 2 мс), а пока FPGA не сконфигурирована задача спит
   `DEF_ZX_TASK_IDLE_MS` за итерацию.

### 3.3 Клавиатура: «Scroll Lock»/«Num Lock» и LED-ы (AVR `zx_mode_switcher`)

Дополнительные функции клавиш повторяют `zx.c` AVR-проекта (там PS/2-коды 0x7E и 0x77; у нас —
HID-коды из `app_km.h`):

| Клавиша | AVR (PS/2) | Здесь (HID, `DEF_KEY_*`) | Действие |
|---|---|---|---|
| **Scroll Lock** | `0x7E` | `DEF_KEY_SCROLL` (0x47) | циклический перебор видео-режимов: `m = modes_register \| ~MODE_VIDEO_MASK; m++; m ^= modes_register; m &= MODE_VIDEO_MASK;` → `zx_mode_switcher(m)` |
| Num Lock | `0x77` | `DEF_KEY_NUM` (0x53) | `zx_mode_switcher(MODE_TAPEOUT)` — вкл/выкл режим tapeout |
| Caps Lock | — (в AVR нет ветки!) | — | в AVR битом `MODE_CAPSLED` управляет **ZX-софт** через регистр C Gluk-часов (`gluk_set_reg`), поэтому локально клавиша не обрабатывается |

`zx_mode_switcher( mode )` (порт AVR, `src/zx.c`):
1. `modes_register ^= mode`;
2. `zx_set_config( ( flags_register & FLAG_LAST_TAPE_VALUE ) ? SPI_TAPE_FLAG : 0 )` — конфигурация
   уходит в ПЛИС в регистр **0x50** (`SPI_CONFIG_REG`): `(modes & MODE_VIDEO_MASK) |
   ((modes & MODE_TAPEOUT) ? SPI_TAPEOUT_MODE_FLAG : 0) | ((flags_ex & FLAG_EX_NMI) ?
   SPI_CONFIG_NMI_FLAG : 0) | (flags & ~…)`, маска статуса `0x7F`;
3. `rtc_write( RTC_COMMON_MODE_REG /*0xFE*/, modes_register )` — режим сохраняется в NVRAM
   (у нас «лишние» ячейки AVR 0xFD..0xFF отображены в свободные BKP DR33..DR35, см. `src/rtc.c`);
4. LED-ы клавиатуры: у AVR здесь отправляется PS/2-команда `SET LED` с байтом
   `(LED_SCROLLOCK|LED_NUMLOCK|LED_CAPSLOCK) & modes_register`; у нас этот байт вычисляется в
   USB-ветке — `KB_AnalyzeKeyValue( )` (как только приходит HID-отчёт) заполняет `SetReport_Value`
   из `modes_register & MODE_LED_MASK`, и `KB_SetReport( )` отправляет его клавиатуре
   (SET_REPORT по управляющей или выходной конечной точке — как описано в отчёте устройства).

Соответствие LED ↔ режим: **bit 0 `MODE_VGA` = «Scroll Lock» LED**, bit 1 `MODE_TAPEOUT` = «Num
Lock» LED, bit 2 `MODE_CAPSLED` = «Caps Lock» LED.

При старте `zx_init( )` дополнительно отправляет `zx_set_config( 0 )`, чтобы ПЛИС получила режим
(VGA/TV) сразу после конфигурации — в логе это видно как `[ZX] config=00 (modes=00)`.

7. **SPI нельзя трогать из контекста с приостановленным планировщиком.** Путь HID-отчётов
   (`KB_AnalyzeKeyValue( )`) выполняется внутри `USBH_MainDeal( )` под `vTaskSuspendAll( )`
   (см. `src/app_usb.c`), поэтому `spi_lock( )` (мьютекс шины) там вызывать нельзя — FreeRTOS
   падает в `configASSERT( "Cannot block if the scheduler is suspended" )` (`queue.c`), что и
   наблюдалось на железе: нажатие Scroll Lock приводило к строке `err at line 1516 of file
   "lib\FreeRTOS\queue.c"` и остановке МК. Решение: такая ветка только выставляет флаг
   `ZxConfigPending`, а сам обмен делает задача `zx` (`zx_service( )`).

---

## 4. Скорость и тайминги

* МК: SPI1 на APB2 = 96 МГц, минимум `/2` → 48 МГц возможно аппаратно, но **дефайн стоит на
  `/16` = 6 МГц** (`DEF_ZX_SPI_PRESCALER`, `SPI_BaudRatePrescaler_16`).
* ПЛИС: `slavespi.v` синхронизирует `spick`/`spido`/`spics_n` своим `fclk` (3-ступенчатые
  регистры + детект фронтов по `spick_sync[2:1]`), поэтому SCK должен быть в разы медленнее
  `fclk` (порядка `fclk/4…fclk/6`). AVR работал на `Fosc/2 = 5.53 МГц`, что как раз
  соответствует `fclk ≈ 21 МГц`. **12 МГц (`SPI_BaudRatePrescaler_8`) без измерения поднимать не
  следует** — скорее всего, это выше предела этого слава; менять ПЛИС для проверки не требуется,
  достаточно одного дефайна.
* Конфигурация RBF: те же пины на 6 МГц (98 023 Б за ≈130 мс), менять её скорость не обязательно.

## 5. Порядок запуска и логи

```
USB-стек → power-задача: PSU on → FPGA_Config( )
    spi_lock → spi_deinit( )            (CS = 1, EXTI7 замаскирован)
    FPGA_ConfigMain( )                  (nCONFIG/nSTATUS/CONF_DONE, 98023 Б по DMA)
    spi_unlock
    zx_init( ) → spi_init( ) → zx_spi_send(0x30,0,0) → gluk_init( )
```
Проверено на железе (лог COM4, 27.09.2026, прошивка ветки `zx_spi`):

```
PWR: POWER_ON asserted (attempt 1)
PWR: POWER_GOOD=1 (343 ms)
USB: stack started (PSU on)
...
FPGA: nSTATUS OK, sending 98023 B
FPGA: sent 98023 B in 131 ms
FPGA: CONF_DONE=1, FPGA configured
SPI: link ready, SCK = 6000 kHz, CS = 1, IRQ = 1
ZX: init done, reset pulse sent, gluk regs 59 34 17 26
[RTOS] zx task started (waiting for the FPGA)
...
[ZXSPI] st=81 port=1 rd addr=04 data=17     (Z80 читает часы: 0x17 = 17 ч)
[ZXSPI] st=81 port=1 rd addr=02 data=35     (минуты 0x35 = 53; ранее 0x34 — время идёт)
[ZXSPI] st=81 port=1 rd addr=00 data=05     (секунды)
[ZXSPI] st=81 port=1 rd addr=07 data=26     (день), 08=09 (месяц), 09=26 (год)
[ZXSPI] st=01 port=1 wr addr=f0 data=03     (запись в расширение 0xF0 — пока не обслуживаем)
```

То есть цепочка «Z80 → wait-порт ПЛИС → `spiint_n` → задача `zx` → SPI → наша модель DS12887 →
обратно в Z80» работает, и Z80 получает реальное время (значения совпадают с `[RTC]` в логе).

## 6. Что дальше

1. ~~Проверка на железе~~ — **выполнено** 27.09.2026 (см. §5): Z80 читает часы через wait-порт и
   получает значения нашей модели.
2. **Измерение потолка** `slavespi`: `DEF_ZX_SPI_PRESCALER` `/16 → /8 → /4` и запись результата
   сюда (пока достаточно обращений Z80, изменения в битстрим не нужны). Сейчас 6 МГц работают.
3. **Расширения ZX-Evolution нужны**: в логе видно обращение Z80 к `0xF0` (`wr addr=f0 data=03`,
   `rd addr=f0 data=00`) — то есть ZX-софт пользуется расширенными адресами. Их надо портировать
   (`DEF_ZX_GLUK_EVO_EXT`): регистр A = адрес EEPROM, регистр C — флаги LED/лога/EEPROM, регистр D —
   состояние клавиш, адреса ≥ 0xF0, ячейки 0xFD..0xFF (в AVR они отображены в NVRAM/EEPROM).
4. **Клавиатурно-мышиный слой**: `SPI_KBD_DAT`/`SPI_KBD_STB` (40 бит), `SPI_MOUSE_*`,
   `SPI_KEMPSTON_JOYSTICK`, `zx_task( )`/`zx_mouse_task( )`, `shift_pause`.
5. **RS232 (Kondratiev)** — ветка `ZXW_KONDR_RS232` не портирована (по согласованию).
6. Мелочь: опрос регистра C (`rd addr=0c`) идёт очень часто — Z80 ждёт флаг обновления; если
   понадобится, можно снизить цену (обслуживание прямо в ISR) или добавить в регистр A бит UIP
   более заметно. Сейчас на работу часов это не влияет.

**История:**

* 26.09.2026 — создан (ветка `zx_spi`): шаг 1 реализован и собран (`RAM 17896 B`,
  `Flash 146844 B`, без предупреждений); протокол восстановлен по AVR-коду и HDL ПЛИС.
