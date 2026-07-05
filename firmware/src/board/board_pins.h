/* board_pins.h — STM32F303VC pin assignments for this hardware revision.
 *
 * Conventions:
 *   GPIOx_PIN_n  where x = port letter, n = pin number
 *   _AF_n        alternate function number (from STM32F303 datasheet Table 14)
 *
 * OQ items are OPEN QUESTION fields still awaiting hardware confirmation.
 */
#pragma once

/* ── SPI1 — shared isoSPI bus ─────────────────────────────────────────────── */
#define SPI_PORT            GPIOA
#define SPI_SCK_PIN         5u      /* PA5  — AF5: SPI1_SCK  */
#define SPI_MISO_PIN        6u      /* PA6  — AF5: SPI1_MISO */
#define SPI_MOSI_PIN        7u      /* PA7  — AF5: SPI1_MOSI */
#define SPI_AF              5u

/* ── SPI1 chip-selects (manual GPIO, active-low, idle HIGH) ──────────────── */
#define CS_CELL_PORT        GPIOA
#define CS_CELL_PIN         4u      /* PA4  — CS for CELL-chain LTC6820 */

#define CS_TEMP_PORT        GPIOB
#define CS_TEMP_PIN         12u     /* PB12 — CS for TEMP-chain LTC6820 */

/* ── USART2 — USB-serial bridge (CP2104) ─────────────────────────────────── */
#define UART_PORT           GPIOA
#define UART_TX_PIN         2u      /* PA2  — AF7: USART2_TX */
#define UART_RX_PIN         3u      /* PA3  — AF7: USART2_RX */
#define UART_AF             7u

/* ── I2C2 — ISL28022 power monitor ───────────────────────────────────────── */
#define I2C_PORT            GPIOA
#define I2C_SCL_PIN         9u      /* PA9  — AF4: I2C2_SCL */
#define I2C_SDA_PIN         10u     /* PA10 — AF4: I2C2_SDA */
#define I2C_AF              4u

/* ISL28022 I2C address: A0 and A1 pins are unconnected (float → pulled low).
 * Default address = 0x40 (A1=0, A0=0). Verify on board before first I2C test. */
#define ISL28022_I2C_ADDR   (0x40u)

/* ── ADC1 — Vpack load-side voltage ─────────────────────────────────────── */
#define VPACK_ADC_PORT      GPIOA
#define VPACK_ADC_PIN       1u      /* PA1  — ADC1_IN2 */
#define VPACK_ADC_CHANNEL   2u
#define VPACK_VREF_MV       3300u

/* ── CAN — vehicle bus (ISO1050) ─────────────────────────────────────────── */
#define CAN_PORT            GPIOA
#define CAN_RX_PIN          11u     /* PA11 — AF9: CAN_RX */
#define CAN_TX_PIN          12u     /* PA12 — AF9: CAN_TX */
#define CAN_AF              9u

/* ── Permission outputs ───────────────────────────────────────────────────── */
/* All four permission outputs (PB10/PB11/PB0/PB2) drive downstream shutdown
 * logic through identical N-channel MOSFET stages (confirmed from schematic):
 *   MCU HIGH → MOSFET on → drain pulled LOW → downstream active-low signal asserted.
 *   MCU LOW  → MOSFET off → downstream pulled HIGH → signal inactive (safe).
 * Safe default at reset = all MCU LOW (board_outputs_init_safe() enforces this). */
#define OUTPUT_PORT_B           GPIOB

#define PIN_CHARGE_ENABLE       0u   /* PB0  — Charge permission output       */
#define PIN_LED0                1u   /* PB1  — Status LED                     */
#define PIN_CHARGER_SAFETY      2u   /* PB2  — Charger safety output          */
#define PIN_POWER_LED           3u   /* PB3  — Power/status LED               */
#define PIN_POWER_ENABLE        5u   /* PB5  — Power latch (keep-alive)       */
#define PIN_DISCHARGE_ENABLE    10u  /* PB10 — Discharge permission; active-low downstream */
#define PIN_MASTER_OK           11u  /* PB11 — MasterOk / multipurpose;       active-low downstream */

/* ── Digital inputs ───────────────────────────────────────────────────────── */
#define PIN_POWER_BUTTON        4u   /* PB4  — Power button (input)           */

#define CHARGE_DETECT_PORT      GPIOC
#define PIN_CHARGE_DETECT       14u  /* PC14 — Charger present (input)        */

/* ── Debug — SWD ──────────────────────────────────────────────────────────── */
/* PA13 SWDIO, PA14 SWCLK — handled by debugger; never configure as GPIO */

/* ── HSE crystal ──────────────────────────────────────────────────────────── */
/* PF0/PF1 OSC_IN/OSC_OUT — configured by RCC; not GPIO */
  