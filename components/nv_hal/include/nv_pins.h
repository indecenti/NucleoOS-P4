// NucleoOS Anima — board pin map (Guition JC1060P420C_I / JC-ESP32P4-M3).
#pragma once

// Display (JD9165, MIPI-DSI 2-lane)
#define NV_PIN_LCD_RESET        5     // GPIO5
#define NV_PIN_LCD_BACKLIGHT    23    // GPIO23 (LEDC PWM)
#define NV_LCD_H_RES            1024
#define NV_LCD_V_RES            600
#define NV_MIPI_DSI_LANES       2
#define NV_MIPI_LANE_MBPS       750
#define NV_MIPI_LDO_CHAN        3     // LDO_VO3 -> VDD_MIPI_DPHY
#define NV_MIPI_LDO_MV          2500
#define NV_DPI_CLK_MHZ          50

// Touch (GT911) + shared internal I2C bus
#define NV_PIN_I2C_SDA          7     // GPIO7
#define NV_PIN_I2C_SCL          8     // GPIO8
#define NV_I2C_HZ               400000
#define NV_PIN_TOUCH_INT        21    // GPIO21
#define NV_PIN_TOUCH_RST        22    // GPIO22
#define NV_GT911_ADDR           0x5D  // primary (0x14 backup clashes with RX8130 RTC — avoid)

// Audio (ES8311 DAC + ES7210 ADC) — later
#define NV_PIN_I2S_MCLK         13
#define NV_PIN_I2S_BCLK         12
#define NV_PIN_I2S_WS           10
#define NV_PIN_I2S_DOUT         9     // -> speaker
#define NV_PIN_I2S_DIN          48    // <- onboard MIC1
#define NV_PIN_AUDIO_PA_EN      11

// SD card (SDMMC 4-bit) — later
#define NV_PIN_SD_CLK           43
#define NV_PIN_SD_CMD           44
#define NV_PIN_SD_D0            39
#define NV_PIN_SD_D1            40
#define NV_PIN_SD_D2            41
#define NV_PIN_SD_D3            42
