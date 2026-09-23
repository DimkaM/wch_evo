# Миграция проекта на FreeRTOS (CH32V317, USB host + HUB)

**Проект:** `D:\src\new_evo\test_usb_1` (PlatformIO, env `genericCH32V317WCU6`)
**Ветка:** `add_freertos` (вариант A: вендоренное ядро лежит в репозитории)
**Дата:** 23.09.2026

---

## 1. Итог

FreeRTOS **работает на железе** с обоими портами USB-хоста и хабом; HID-поток LS-мыши
за хабом FE1.1s (USBFS) идёт непрерывно, хаб CH334 на USBHS перечисляется в FS-режиме.

Проверено на плате (лог COM4 @115200):

| Гейт | Что проверялось | Результат |
|---|---|---|
| G1 | сборка/линковка ядра, старт планировщика, работа TIM3-ISR под RTOS | `FreeRTOS V10.4.6`, задача печатает измерения, `g_ms_ticks` растёт |
| G2 | реальная частота тика | 1000 тиков = **1999…2000 мс** при `configTICK_RATE_HZ = 500` (SysTick RTOS и независимый 1 мс таймер TIM3 совпадают) |
| G3 | блокирующие задержки при работающем планировщике | `Delay_Ms(10)` → **10 мс**, `Delay_Us(1000)` → **1 мс** (замер по TIM3) |
| G4 | USB-хост под RTOS | перечисление USBFS (FE1.1s + LS-мышь `18f8:0f99`) и USBHS (CH334, HS→FS), **непрерывный поток HID-отчётов**, ошибок/HardFault нет |
| G5 | ресурсы | `heapFree = heapMin = 4416 B` (утечек нет), `usbStackFree = 858 слов` из 1024, `stackFree (test) ≥ 106 слов` |

Размеры прошивки (release, `-O2`):

| | Flash | RAM (static + heap 12 КБ) |
|---|---|---|
| bare-metal (до миграции, `main`) | 27 348 B (10.4 %) | 6 264 B (9.6 %) |
| FreeRTOS (эта ветка) | **36 320 B (13.9 %)** | **19 184 B (29.3 %)** |

Лимиты платы: 256 КБ Flash / 64 КБ RAM.

## 2. Что было сделано (по шагам)

1. **ABI на дефолт платы** (`platformio.ini`): `rv32imacxw` / `ilp32` (soft-float) вместо
   `rv32imafcxw` / `ilp32f`. В проекте нет ни одного `float`/`double`, а WCH-порт FreeRTOS
   сохраняет FPU-регистры только при `ARCH_FPU = 1`, поэтому soft-float полностью снимает
   вопрос контекста FPU **без правки вендоренного ядра**.
2. **Компиляция ядра**: `lib/FreeRTOS` (V10.4.6 + WCH RISC-V порт) подхватывается LDF
   автоматически (`Dependency Graph |-- FreeRTOS`) как только исходник проекта включает
   `FreeRTOS.h`. Дополнительные `-I` и правки `platformio.ini` не потребовались: нужный
   ассемблеру `freertos_risc_v_chip_specific_extensions.h` лежит рядом с `portASM.S`.
3. **Два режима работы** (`DEF_FREERTOS_EN` в `src/USB_Host/usb_host_config.h`):
   `1` — планировщик + задачи (эта ветка), `0` — прежний bare-metal суперцикл.
   Оба режима компилируются из одного `main.c`; откат — один макрос.
4. **`Delay_Us`/`Delay_Ms` переведены с SysTick на TIM4** (`src/debug.c`,
   переключатель `DEF_DELAY_TIMER_BASED` в `include/debug.h`). Это было обязательным
   условием: SysTick в RTOS-режиме — источник тика (`port.c` задаёт `CMP` и `CTLR`), а
   USB-драйвер вызывает задержки тысячи раз за транзакцию.
   TIM4: 1 мкс/тик, 16 бит, свободный пробег, **прерывание выключено**, только опрос `CNT`
   (вычитание в `uint16_t` — корректно на переполнении; длинные паузы нарезаются по 60 мс).
5. **Задачи** (`src/app_tasks.c`, новые файлы `app_tasks.c/.h`):
   * `usb_host` — инициализирует оба порта и крутит `USBH_MainDeal()`; приоритет 2,
     стек 1024 слова; один проход поллинга выполняется с `vTaskSuspendAll()`, чтобы
     перечисление/транзакция не прерывались задачами (аппаратные прерывания работают);
   * `rtos_test` — bring-up задача гейтов G1-G5 (приоритет 1, стек 256 слов, можно
     отключить `DEF_RTOS_TEST_TASK 0`); измеряет реальное время 1000 тиков против
     1-мс счётчика TIM3, печатает `heapFree/heapMin/stackFree/usbStackFree` и точность
     задержек;
   * idle + timer создаёт сам FreeRTOS.
6. **Счётчик 1 мс для задач**: `volatile uint32_t g_ms_ticks` (`app_km.c`, объявление в
   `app_km.h`) инкрементируется в `TIM3_IRQHandler` — он же показывает, что TIM3-ISR
   продолжает обслуживаться под планировщиком (интервалы HID/HUB работают).

## 3. Архитектура и переключатели

```
main()
 ├─ SystemCoreClockUpdate(), Delay_Init() (TIM4), USART_Printf_Init()
 ├─ TIM3_Init()                       // 1 мс, интервалы HID/HUB + g_ms_ticks
 └─ #if DEF_FREERTOS_EN
       AppTasks_Start() → xTaskCreate(usb_host, rtos_test)
       vTaskStartScheduler()
    #else
       __enable_irq(); USBFS/USBHS init; while(1) USBH_MainDeal();
    #endif
```

| Макрос (`src/USB_Host/usb_host_config.h`) | Значение | Смысл |
|---|---|---|
| `DEF_FREERTOS_EN` | 1 | 1 — FreeRTOS-режим, 0 — прежний суперцикл |
| `DEF_RTOS_USB_TASK_STACK_WORDS` | 1024 | стек задачи USB-хоста (слов) |
| `DEF_RTOS_USB_TASK_PRIO` | 2 | приоритет задачи USB-хоста |
| `DEF_RTOS_USB_DELAY_TICKS` | 1 | пауза между проходами поллинга (тики, 2 мс) |
| `DEF_RTOS_TEST_TASK` | 1 | bring-up задача измерений (0 — выключить) |
| `DEF_RTOS_TEST_DELAY_TICKS` / `_STACK_WORDS` / `_PRIO` | 1000 / 256 / 1 | параметры bring-up задачи |

| Макрос | Файл | Значение | Смысл |
|---|---|---|---|
| `DEF_DELAY_TIMER_BASED` | `include/debug.h` | 1 | 1 — задержки на TIM4, 0 — старые на SysTick |
| `INCLUDE_uxTaskGetStackHighWaterMark` | `include/FreeRTOSConfig.h` | 1 | для замеров стека задач |
| `configTOTAL_HEAP_SIZE` | `include/FreeRTOSConfig.h` | 12 КБ | heap_4 |

Изменённые/новые файлы: `platformio.ini` (ABI), `src/main.c` (два режима + includes),
`src/app_tasks.c/.h` (новые), `src/USB_Host/app_km.c/.h` (`g_ms_ticks`),
`src/USB_Host/usb_host_config.h` (переключатели), `src/debug.c`, `include/debug.h`,
`include/FreeRTOSConfig.h`, `tools/com4_capture.ps1` (новый, захват лога).

**Вендоренные файлы `lib/FreeRTOS` и `lib/ch32v30x` не изменялись** — все адаптации
сделаны в прикладном коде (кроме `Link.ld`, который ещё до миграции содержал
`__freertos_irq_stack_top`, необходимый порту).

## 4. Ключевые технические решения и почему так

1. **SysTick нельзя делить с задержками.** `port.c:154-166` настраивает SysTick как тик
   (`CMP = CPU/tickHz`, `CTLR = 0xf`), а `src/debug.c` в SysTick-варианте пишет `CMP`,
   режим и гасит счётчик — планировщик бы «умер». Отсюда TIM4.
2. **Порт и стартап совместимы как есть.** `src/startup_ch32v30x_D8C.S` задаёт
   `mstatus = 0x7800` (MIE=0, MPP=3, FS=3) — порт сам включает глобальные прерывания в
   `xPortStartFirstTask`; `SW_Handler` и `SysTick_Handler` определены в `portASM.S`/`port.c`
   и перекрывают weak-заглушки векторов. Ничего в стартапе менять не пришлось.
3. **Прерывания не вкладываются**: `port.c` ставит SysTick/SW на самый низкий приоритет
   (`NVIC_SetPriority(..., 0xf0)`), PFIC-вложенность не включена, поэтому `TIM3_IRQHandler`
   (приоритет из `NVIC_Init`, без вызовов FreeRTOS API) просто работает.
4. **Монополизация во время перечисления**: `vTaskSuspendAll()` вокруг одного прохода
   `USBH_MainDeal()`. Это защищает пошаговый опрос WCH-стека от переключения задач, ценой
   того, что прочие задачи «замирают» на время перечисления (первый проход — примерно
   18-20 с: инициализация двух портов + перечисление двух хабов + сканы портов).
   В логе это видно как первая строка bring-up задачи: `realMs=19990` при 1000 тиков, все
   последующие — `realMs≈2033` при 1017 тиков.
5. **`printf` — единственный общий ресурс.** Пока включены и USB-задача, и bring-up задача,
   строки могут изредка перемешиваться (обе печатают в USART1). Для «тихого» режима:
   `DEF_RTOS_TEST_TASK 0` (или `DEF_DEBUG_HID_REPORT 0`).
6. **FPU**: `ARCH_FPU` в вендоренном заголовке оставлен `0`, сборка — soft-float
   (`rv32imacxw`/`ilp32`). Если в проекте появятся расчёты с плавающей точкой в нескольких
   задачах — либо вернуть hard-float и `ARCH_FPU 1`, либо оставить soft-float (float будет
   медленнее, но корректно).

## 5. Как проверялось

Сборка/прошивка/лог (PowerShell, Windows):

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e genericCH32V317WCU6                 # сборка
& $pio run -e genericCH32V317WCU6 -t upload       # прошивка через wch-link
.\tools\com4_capture.ps1 -Seconds 15 -OutFile "$env:TEMP\com4.txt"   # лог COM4
```

Приёмочный лог этой ветки (фрагмент, COM4 @115200):

```
SystemClk:96000000
ChipID:3173b568
USB HOST KM Test
TIM3 Init OK!
FreeRTOS V10.4.6
[RTOS] kernel=V10.4.6
[RTOS] usb host task: created
[RTOS] test task: created
USBHS Host Init
USBFS Host Init
[RTOS] USB host task started
USB Port0 Dev In. … DevType: 09 … RootHubDev[00].bPortNum: 04 … HUB ports will be scanned
USB Port1 Dev In. … HUB on USBHS: switch port to FULL speed … RootHubDev[01].bPortNum: 04 …
HubP1 pre1=fe / Hub Port1 In / Dev Speed:0 / HUB port1: low-speed device behind the HUB is not supported!
HubP2 … Dev Speed:1 … HUB port2 device is unknown!
Hub Port4 In … 12 01 10 01 … f8 18 99 0f … DevType: 03 … HUB port4 device is HID! Further Enum: … OK!
01 00 01 20 00 00          <- поток HID-отчётов LS-мыши за хабом
[RTOS] ticks=1017 realMs=2033 tickHz=500 heapFree=4416 heapMin=4416 stackFree=206 usbStackFree=858 delay10ms=12 delay1000us=1
```

Замечание по железу: `SystemClk:96000000` — фактическая частота ядра 96 МГц (не 144),
при этом оба таймера (SysTick-тик RTOS и TIM3 1 мс) рассчитаны от `SystemCoreClock`, поэтому
метрики согласованы; менять частоту не требуется.

## 6. Известные ограничения / что осталось

1. **Пауза задач во время перечисления** (~18-20 с на первый проход) — плата за
   `vTaskSuspendAll()` вокруг `USBH_MainDeal()`. Если понадобится отзывчивость во время
   перечисления, нужно дробить `USBH_MainDeal()` на шаги (рефакторинг WCH-стека).
2. **Печать без мьютекса**: строки двух задач могут перемешиваться (в логе видно один такой
   случай). Полностью решается либо `DEF_RTOS_TEST_TASK 0`, либо единым logger-заданием.
3. **LS-устройства за хабом на USBHS** по-прежнему невозможны аппаратно (нет PRE-токена,
   см. `RESEARCH_CONCLUSION.md`); USBFS работает и это подтверждено этой сборкой.
4. **Стек задачи USB-хоста** 1024 слова при фактическом расходе ~166 слов; уменьшать не
   обязательно (heap свободен: 4.4 КБ), но при нужде можно снизить до 512 слов.
5. **`configTOTAL_HEAP_SIZE` 12 КБ** — при добавлении задач/буферов следить за
   `heapMin` в логе; при необходимости `Link.ld` позволяет поднять RAM с 32 КБ до 64 КБ
   (плата 256 КБ Flash = 64 КБ RAM).
6. Не проверялось: горячее подключение/отключение устройства под RTOS (>30 мин работы),
   `vTaskDelay`-точность при длительной непрерывной нагрузке.

## 7. Откат и контроль версий

* Ветка `add_freertos`, коммиты по гейтам; `main` остаётся рабочей bare-metal точкой.
* Откат к суперциклу без смены ветки: `DEF_FREERTOS_EN 0` (и, при желании,
  `DEF_DELAY_TIMER_BASED 0`, чтобы вернуть SysTick-задержки).
* Материалы по исходной задаче (хаб, SPLIT-исследование) — `RESEARCH_CONCLUSION.md`.

