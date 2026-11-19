# Pin map

Pins and peripherals as they are set up in `CommunicationBoard.ioc` for the
**Nucleo-G474RE** dev board. The Comms V1 column is the proposal from the firmware
architecture document and is not final until the schematic is.

In code, the pin map lives in `App/board/board.h` and the labels CubeMX generates into
`Core/Inc/main.h`.

| Function | Peripheral | Nucleo-G474RE (set up now) | Comms V1 (proposed) |
|---|---|---|---|
| CAN RX / TX | FDCAN2 | PB12 / PB13 | PB12 / PB13 to the transceiver |
| SD SCK / MISO / MOSI | SPI1 + DMA | PA5 / PA6 / PA7. PA6 has its pull-up on. | PA5 / PA6 / PA7, 10k pull-up on PA6 |
| SD chip select | GPIO output | PA4 (`SD_CS`), starts high | PA4 |
| Card detect | EXTI | Not set up yet (chapter 6) | PB0, pull-up |
| SD power switch | GPIO | None | PB2 |
| Power-fail input | COMP1 non-inverting input | PA1. Configured, not started (chapter 6). | PA1 from the input divider |
| Debug UART | USART2, 115200 baud | PA2 / PA3 (ST-LINK virtual COM port) | PA2 / PA3 to a header |
| USB D- / D+ | USB FS | PA11 / PA12. Initialised, not used (chapter 8). | PA11 / PA12 to USB-C |
| LED heartbeat / CAN / SD / fault | GPIO output | PC0 / PC1 / PC2 / PC3 (external LEDs) | PC0 to PC3 |
| Debug | SWD | PA13 / PA14 (on-board ST-LINK) | PA13 / PA14 header |
| RTC crystal | LSE, 32.768 kHz | PC14 / PC15 | 32.768 kHz crystal |
| Main crystal | HSE | PF0 / PF1, 24 MHz | Frequency to be decided with hardware |
| Do not use | | PB8 (BOOT0). LD2 on PA5 flickers with SPI traffic. | PB8 |

## Clocks

| Clock | Value |
|---|---|
| System clock | 170 MHz from the PLL (HSE 24 MHz, PLLM /6, PLLN x85, PLLR /2) |
| FDCAN | 170 MHz. 1 Mbps = prescaler 10, 17 time quanta per bit, sample point 88%. |
| SPI1 (APB2) | 85 MHz. /256 = 332 kHz for card start-up, /4 = 21.25 MHz for data. |
| TIM2 | 1 MHz (prescaler 169), 32-bit, free-running: the microsecond clock |
| Watchdog (IWDG) | About 2 s (LSI / 32, reload 1999) |
| USB | 48 MHz from HSI48 with CRS |
| RTC | LSE |

Moving to the real board means changing the HSE frequency and PLLM in CubeMX to match
its crystal.

## Interrupt priorities

Lower number = more urgent. Set in CubeMX and again in `board_init()`.

| Interrupt | Priority | Does |
|---|---|---|
| COMP1 (power-fail) | 5 | Chapter 6 |
| FDCAN2 | 6 | Timestamps and copies every frame into the RX ring |
| SPI1 DMA | 7 | Marks an SD block transfer as finished |
| Card detect (EXTI) | 8 | Chapter 6 |
| TIM2 overflow | 9 | Extends the microsecond clock to 64 bits |
| USART2 and its DMA | 10 | Debug output |
