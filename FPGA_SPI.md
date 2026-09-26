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
Ожидаемый лог:

```
FPGA: CONF_DONE=1, FPGA configured
SPI: link ready, SCK = 6000 kHz, CS = 1, IRQ = 1
ZX: init done, reset pulse sent, gluk regs SS MM HH DD
...
[ZXSPI] st=81 port=1 rd addr=00 data=SS     (Z80 читает Gluk-регистр)
[ZXSPI] st=01 port=1 wr addr=0B data=02     (Z80 пишет Gluk-регистр)
GLUK: write to the index 4x ignored (...)   (если Z80-софт полез в расширения)
```

## 6. Что дальше

1. **Проверка на железе**: Z80-софт, обращающийся к Gluk-часам — ожидаем строки `[ZXSPI]`, чтение
   времени должно совпадать с `[RTC]` в логе, запись — менять его.
2. **Измерение потолка** `slavespi`: `DEF_ZX_SPI_PRESCALER` `/16 → /8 → /4` и запись результата
   сюда (пока достаточно обращений Z80, изменения в битстрим не нужны).
3. **Клавиатурно-мышиный слой**: `SPI_KBD_DAT`/`SPI_KBD_STB` (40 бит), `SPI_MOUSE_*`,
   `SPI_KEMPSTON_JOYSTICK`, `zx_task( )`/`zx_mouse_task( )`, `shift_pause` — и вместе с ним
   расширения Gluk (регистры A/C/D, адреса ≥ 0xF0).
4. **RS232 (Kondratiev)** — ветка `ZXW_KONDR_RS232` не портирована (по согласованию).

**История:**

* 26.09.2026 — создан (ветка `zx_spi`): шаг 1 реализован и собран (`RAM 17896 B`,
  `Flash 146844 B`, без предупреждений); протокол восстановлен по AVR-коду и HDL ПЛИС.
