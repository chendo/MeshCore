#include <Arduino.h>
#include <Wire.h>

#include "ThinkNodeM1Board.h"

#ifdef THINKNODE_M1

#ifdef NRF52_POWER_MANAGEMENT
// Static configuration for power management
// Values come from variant.h defines
const PowerMgtConfig power_config = {
  .lpcomp_ain_channel = PWRMGT_LPCOMP_AIN,
  .lpcomp_refsel = PWRMGT_LPCOMP_REFSEL,
  .voltage_bootlock = PWRMGT_VOLTAGE_BOOTLOCK
};

void ThinkNodeM1Board::initiateShutdown(uint8_t reason) {
  // Reached from checkBootVoltage() inside begin(), before the radio, display
  // and GPS drivers exist, so this stays on raw GPIO and must never call the
  // driver-level teardown in shutdownPeripherals().

  // SX126X_POWER_EN is also PIN_GPS_RESET on this board, so dropping it cuts the
  // radio rail and holds the GPS receiver in reset in one go.
  pinMode(SX126X_POWER_EN, OUTPUT);
  digitalWrite(SX126X_POWER_EN, LOW);
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, LOW);

  // Release everything feeding the now-dead radio rail; a pin left driven high
  // back-powers the SX1262 through its ESD diodes.
  nrf_gpio_cfg_default(PIN_TXCO);
  nrf_gpio_cfg_default(P_LORA_NSS);
  nrf_gpio_cfg_default(P_LORA_RESET);
  nrf_gpio_cfg_default(P_LORA_DIO_1);
  nrf_gpio_cfg_default(P_LORA_BUSY);
  nrf_gpio_cfg_default(P_LORA_SCLK);
  nrf_gpio_cfg_default(P_LORA_MISO);
  nrf_gpio_cfg_default(P_LORA_MOSI);

  // E-ink keeps its last image with no rail behind it; park the panel and the
  // SPI1 bus it shares with LED_RED (DISP_MISO).
  digitalWrite(DISP_BACKLIGHT, LOW);   // preload before enabling the driver
  pinMode(DISP_BACKLIGHT, OUTPUT);
  nrf_gpio_cfg_default(DISP_CS);
  nrf_gpio_cfg_default(DISP_DC);
  nrf_gpio_cfg_default(DISP_RST);
  nrf_gpio_cfg_default(DISP_BUSY);
  nrf_gpio_cfg_default(DISP_MISO);
  nrf_gpio_cfg_default(DISP_MOSI);
  nrf_gpio_cfg_default(DISP_SCLK);

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif
  digitalWrite(LED_GREEN, !LED_STATE_ON);
  digitalWrite(LED_BLUE, !LED_STATE_ON);

  // PIN_PWR_EN stays high on purpose. The 150K/150K divider on PIN_VBAT_READ is
  // the LPCOMP input, and nothing here proves that rail does not gate it —
  // a few uA of divider leakage is worth a wake that can still see the battery.

  bool enable_lpcomp = (reason == SHUTDOWN_REASON_LOW_VOLTAGE ||
                        reason == SHUTDOWN_REASON_BOOT_PROTECT);
  if (enable_lpcomp) {
    configureVoltageWake(power_config.lpcomp_ain_channel, power_config.lpcomp_refsel);
  }

  enterSystemOff(reason);
}
#endif // NRF52_POWER_MANAGEMENT

void ThinkNodeM1Board::begin() {
  NRF52Board::begin();

#ifdef NRF52_POWER_MANAGEMENT
  // Boot voltage protection check (may not return if voltage too low).
  // Runs before the radio rail comes up, so the divider is read under the
  // lightest load the board ever presents — the reading errs high, which is the
  // side that cannot lock a healthy cell out.
  checkBootVoltage(&power_config);
#endif

  Wire.begin();

#ifdef P_LORA_TX_LED
  pinMode(P_LORA_TX_LED, OUTPUT);
  digitalWrite(P_LORA_TX_LED, LOW);
#endif

  pinMode(SX126X_POWER_EN, OUTPUT);
  digitalWrite(SX126X_POWER_EN, HIGH);
  delay(10); // give sx1262 some time to power up
}

uint16_t ThinkNodeM1Board::getBattMilliVolts() {
  int adcvalue = 0;

  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  delay(10);

  // ADC range is 0..3000mV and resolution is 12-bit (0..4095)
  adcvalue = analogRead(PIN_VBAT_READ);
  // Convert the raw value to compensated mv, taking the resistor-
  // divider into account (providing the actual LIPO voltage)
  return (uint16_t)((float)adcvalue * REAL_VBAT_MV_PER_LSB);
}
#endif
