# Конфигурация FPGA (Altera, passive serial по SPI1 + DMA)

**Проект:** `D:\src\new_evo\test_usb_1`, ветка `conf_fpga`
**Источник кода:** рабочий проект `D:\src\new_evo\test_altera` (`src/fpga.c`) — последовательность
перенесена без изменений; добавлена только обвязка «после старта» и логи.

---

## 1. Что и когда происходит

Конфигурация выполняется **один раз при старте**, после того как поднимется USB-хост-стек:

```
планировщик → usb_host (prio 2, перечисляет USBFS: хаб FE1.1s и устройства за ним)
             → rtos_test (prio 1)
             → fpga_cfg (prio 4): спит DEF_FPGA_CONFIG_DELAY_MS = 3000 мс,
                                  затем заливает RBF и удаляет себя (vTaskDelete)
```

Задержка нужна, чтобы к моменту конфигурации USB уже работал: в дальнейшем по клавиатуре
за хабом планируется обработка hot keys (сейчас — только публикуются флаги готовности
`g_usbRootReady` / `g_usbHidKbReady`, задача печатает их значение).

После конфигурации задача исчезает — повторных заливок нет.

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
FPGA: configuration in 3000 ms
… (перечисление USB) …
FPGA: start (usbRoot=1 usbHidKb=0)
FPGA: nSTATUS OK, sending 98023 B
FPGA: sent 98023 B in NNN ms
FPGA: CONF_DONE=1, FPGA configured
```

Ошибки: `FPGA: nSTATUS timeout (N ms)`, `FPGA: nSTATUS lost after N B`,
`FPGA: CONF_DONE timeout`, `FPGA: configuration FAILED`.

## 7. Режимы проекта

* **FreeRTOS** (`DEF_FREERTOS_EN 1`) — задача `fpga_cfg` (описано выше).
* **Bare-metal** (`DEF_FREERTOS_EN 0`) — в суперцикле `main()`: как только
  `g_usbRootReady` станет 1, однократно вызывается `FPGA_Config()`.

## 8. Ресурсы и ограничения

* Flash: было 31 632 Б → ≈ **130 КБ** из 256 КБ (битстрим 98 КБ).
* RAM: без изменений; куча FreeRTOS: +1 КБ на задачу (следить за `heapMin` в логе).
* Между блоками DMA возможны короткие паузы DCLK (окна `vTaskSuspendAll()` USB-задачи);
  Altera PS это допускает. Если понадобится — увеличить `DEF_FPGA_CONFIG_BLOCK_SIZE`.
* Hot keys (по клавиатуре за хабом) — точка расширения: флаги готовности уже публикуются
  (`src/USB_Host/app_km.h`), логику проверки клавиш добавлять в `FPGA_ConfigTask()`.
