/*
 * cec_transceiver.c
 *
 * Implements cec_transceiver.h by adapting the CEC bit-banging RX/TX
 * state machines and EDID/DDC physical-address discovery from the upstream
 * gkoh/pico-cec project (src/cec-frame.c, src/ddc.c).
 *
 * CEC RX: edge-interrupt driven state machine
 * CEC TX: alarm-interrupt (hardware timer) driven state machine
 * DDC/EDID: I2C master read from HDMI sink at address 0x50
 *
 * Designed for bare-metal (no FreeRTOS) — task notifications replaced with
 * flags polled from the main loop.
 */

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "cec_transceiver.h"

/* ---- Internal helpers ------------------------------------------------- */

#define HEADER0(iaddr, daddr) (((iaddr) << 4) | (daddr))
#define CEC_FRAME_MAX_LEN     16
#define EDID_BLOCK_SIZE       128
#define EDID_I2C_TIMEOUT_US   (100 * 1000)
#define EDID_I2C_ADDR         0x50
#define EDID_I2C_READ_SIZE    (EDID_BLOCK_SIZE * 2)
#define EDID_I2C_RETRIES      3
#define EDID_CTA_DTD_START    0x02
#define EDID_CTA_DBC_OFFSET   0x04
#define I2C_MASTER_FREQUENCY  (50 * 1000)

/* ---- RX state machine states ------------------------------------------ */

enum {
  RX_STATE_START_LOW = 0,
  RX_STATE_START_HIGH,
  RX_STATE_DATA_LOW,
  RX_STATE_DATA_HIGH,
  RX_STATE_EOM_LOW,
  RX_STATE_EOM_HIGH,
  RX_STATE_ACK_LOW,
  RX_STATE_ACK_HIGH,
  RX_STATE_ACK_END,
  RX_STATE_END,
  RX_STATE_ABORT
};

/* ---- TX state machine states ------------------------------------------ */

enum {
  TX_STATE_START_LOW = 0,
  TX_STATE_START_HIGH,
  TX_STATE_DATA_LOW,
  TX_STATE_DATA_HIGH,
  TX_STATE_EOM_LOW,
  TX_STATE_EOM_HIGH,
  TX_STATE_ACK_LOW,
  TX_STATE_ACK_HIGH,
  TX_STATE_ACK_WAIT,
  TX_STATE_END,
  TX_STATE_DONE
};

/* ---- Static state ------------------------------------------------------ */

static uint cec_gpio;

static struct {
  uint8_t data[CEC_FRAME_MAX_LEN];
  uint8_t len;
  uint8_t byte;
  uint8_t bit;
  uint8_t address;
  uint8_t state;
  bool eom;
  bool ack;
  bool first;
  uint64_t start;
  bool frame_ready;
  bool abort;
} rx;

static struct {
  uint8_t data[CEC_FRAME_MAX_LEN];
  uint8_t len;
  uint8_t byte;
  uint8_t bit;
  uint8_t state;
  bool ack;
  bool broadcast;
  uint64_t start;
  bool done;
} tx;

static uint ddc_scl_gpio;
static uint ddc_sda_gpio;
static uint hpd_gpio;
static const char *last_discovery_error = "not_run";

/* ---- Forward declarations --------------------------------------------- */

static void cec_rx_start(void);

/* ---- GPIO helpers ----------------------------------------------------- */

static inline void cec_output_low(void) {
  gpio_set_dir(cec_gpio, GPIO_OUT);
  gpio_put(cec_gpio, 0);
}

static inline void cec_release(void) {
  gpio_set_dir(cec_gpio, GPIO_IN);
}

/* ---- Timer helper ----------------------------------------------------- */

static uint64_t time_until(uint64_t start, uint64_t delay_us) {
  int64_t remaining = delay_us - (int64_t)(time_us_64() - start);
  return remaining > 0 ? (uint64_t)remaining : 0;
}

/* ---- RX ACK release callback ------------------------------------------ */

static int64_t ack_high(alarm_id_t alarm, void *user_data) {
  (void)alarm;
  (void)user_data;
  cec_release();
  return 0;
}

/* ---- RX ISR (edge-interrupt driven) ----------------------------------- */

static void cec_rx_isr(uint gpio, uint32_t events) {
  gpio_acknowledge_irq(gpio, events);
  gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

  uint64_t now = time_us_64();

  switch (rx.state) {
    case RX_STATE_START_LOW:
      rx.start = now;
      rx.state = RX_STATE_START_HIGH;
      gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_RISE, true);
      return;

    case RX_STATE_START_HIGH: {
      uint64_t low = now - rx.start;
      if (low >= 3500 && low <= 3900) {
        rx.first = true;
        rx.byte = 0;
        rx.bit = 0;
        rx.state = RX_STATE_DATA_LOW;
        gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_FALL, true);
      } else {
        rx.state = RX_STATE_ABORT;
        rx.abort = true;
      }
      return;
    }

    case RX_STATE_EOM_LOW:
      rx.byte++;
      rx.bit = 0;
      // fall through
    case RX_STATE_DATA_LOW: {
      uint64_t min_time = rx.first ? 4300 : 2050;
      uint64_t max_time = rx.first ? 4700 : 2750;
      uint64_t bit_time = now - rx.start;
      if (bit_time >= min_time && bit_time <= max_time) {
        rx.start = now;
        rx.state = (rx.state == RX_STATE_EOM_LOW) ? RX_STATE_EOM_HIGH
                                                   : RX_STATE_DATA_HIGH;
        rx.first = false;
        gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_RISE, true);
      } else {
        rx.state = RX_STATE_ABORT;
        rx.abort = true;
      }
      return;
    }

    case RX_STATE_EOM_HIGH:
    case RX_STATE_DATA_HIGH: {
      uint64_t low = now - rx.start;
      uint8_t bit;
      if (low >= 400 && low <= 800) {
        bit = 1;
      } else if (low >= 1300 && low <= 1700) {
        bit = 0;
      } else {
        rx.state = RX_STATE_ABORT;
        rx.abort = true;
        return;
      }

      if (rx.state == RX_STATE_EOM_HIGH) {
        rx.eom = bit;
        rx.state = RX_STATE_ACK_LOW;
      } else {
        rx.data[rx.byte] = (rx.data[rx.byte] << 1) | bit;
        rx.bit++;
        if (rx.bit > 7) {
          rx.state = RX_STATE_EOM_LOW;
        } else {
          rx.state = RX_STATE_DATA_LOW;
        }
      }
      gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_FALL, true);
      return;
    }

    case RX_STATE_ACK_LOW:
      rx.start = now;
      {
        uint8_t tgt = rx.data[0] & 0x0f;
        if (tgt != 0x0f && tgt == rx.address) {
          rx.state = RX_STATE_ACK_END;
          cec_output_low();
          add_alarm_at(from_us_since_boot(now + 1500), ack_high, NULL, true);
          rx.ack = true;
        }
        rx.state = RX_STATE_ACK_HIGH;
      }
      gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_RISE, true);
      return;

    case RX_STATE_ACK_HIGH: {
      uint64_t low = now - rx.start;
      if ((low >= 400 && low <= 800) || (low >= 1300 && low <= 1700)) {
        rx.state = RX_STATE_ACK_END;
      } else {
        rx.state = RX_STATE_ABORT;
        rx.abort = true;
        return;
      }
      // fall through
    }
    case RX_STATE_ACK_END:
      if (rx.eom) {
        rx.state = RX_STATE_END;
      } else {
        rx.state = RX_STATE_DATA_LOW;
        gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_FALL, true);
        return;
      }
      // fall through
    case RX_STATE_END:
    default:
      rx.len = rx.byte;
      rx.frame_ready = true;
      return;
  }
}

/* ---- Re-arm RX listener ----------------------------------------------- */

static void cec_rx_start(void) {
  rx.state = RX_STATE_START_LOW;
  rx.ack = false;
  rx.abort = false;
  rx.frame_ready = false;
  rx.len = 0;
  // rx.address is preserved across re-arms — set once by
  // cec_claim_logical_address() or cec_transceiver_init().
  memset(rx.data, 0, sizeof(rx.data));
  gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_FALL, true);
}

/* ---- TX alarm callback ------------------------------------------------- */

static int64_t cec_tx_alarm(alarm_id_t alarm, void *user_data) {
  (void)alarm;
  (void)user_data;

  switch (tx.state) {
    case TX_STATE_START_LOW:
      cec_output_low();
      tx.start = time_us_64();
      tx.state = TX_STATE_START_HIGH;
      return time_until(tx.start, 3700);

    case TX_STATE_START_HIGH:
      cec_release();
      tx.state = TX_STATE_DATA_LOW;
      return time_until(tx.start, 4500);

    case TX_STATE_DATA_LOW:
      cec_output_low();
      tx.start = time_us_64();
      {
        uint64_t low_us =
            (tx.data[tx.byte] & (1 << tx.bit)) ? 600 : 1500;
        tx.state = TX_STATE_DATA_HIGH;
        return time_until(tx.start, low_us);
      }

    case TX_STATE_DATA_HIGH:
      cec_release();
      if (tx.bit--) {
        tx.state = TX_STATE_DATA_LOW;
      } else {
        tx.byte++;
        tx.state = TX_STATE_EOM_LOW;
      }
      return time_until(tx.start, 2400);

    case TX_STATE_EOM_LOW:
      cec_output_low();
      {
        uint64_t low_us = (tx.byte < tx.len) ? 1500 : 600;
        tx.start = time_us_64();
        tx.state = TX_STATE_EOM_HIGH;
        return time_until(tx.start, low_us);
      }

    case TX_STATE_EOM_HIGH:
      cec_release();
      tx.state = TX_STATE_ACK_LOW;
      return time_until(tx.start, 2400);

    case TX_STATE_ACK_LOW:
      cec_output_low();
      tx.start = time_us_64();
      tx.state = TX_STATE_ACK_HIGH;
      return time_until(tx.start, 600);

    case TX_STATE_ACK_HIGH:
      cec_release();
      if (tx.byte < tx.len) {
        tx.bit = 7;
        tx.state = TX_STATE_DATA_LOW;
        return time_until(tx.start, 2400);
      } else {
        tx.state = TX_STATE_ACK_WAIT;
        return time_until(tx.start, (850 + 1250) / 2);
      }

    case TX_STATE_ACK_WAIT:
      if (tx.broadcast) {
        tx.ack = gpio_get(cec_gpio);
      } else {
        tx.ack = !gpio_get(cec_gpio);
      }
      tx.state = TX_STATE_END;
      return time_until(tx.start, 2400);

    case TX_STATE_END:
    default:
      tx.done = true;
      return 0;
  }
}

/* ---- Transmit one frame (blocking) ------------------------------------- */

static bool cec_frame_send_raw(const uint8_t *msg, uint8_t len) {
  bool is_broadcast = ((msg[0] & 0x0F) == 0x0F);

  // Wait 7 bit-times of idle bus, with timeout to avoid hanging if a
  // device (e.g. a TV going to standby) holds the bus low.
  unsigned i = 0;
  uint64_t idle_start = time_us_64();
  while (i < 7) {
    busy_wait_us(2400);
    if (gpio_get(cec_gpio)) {
      i++;
    } else {
      i = 0;
    }
    if (time_us_64() - idle_start > 50000) break;
  }

  // Disable RX while transmitting
  gpio_set_irq_enabled(cec_gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);

  tx.done = false;
  tx.ack = false;
  tx.broadcast = is_broadcast;
  tx.byte = 0;
  tx.bit = 7;
  tx.start = 0;
  tx.state = TX_STATE_START_LOW;
  tx.len = len;
  memcpy(tx.data, msg, len);

  add_alarm_at(from_us_since_boot(time_us_64()), cec_tx_alarm, NULL, true);

  uint64_t tx_start = time_us_64();
  while (!tx.done) {
    if (time_us_64() - tx_start > 200000) {
      /* TX state machine hung (bus stuck or alarm lost) — force done. */
      tx.done = true;
      tx.ack  = false;
      cec_release();
    }
    tight_loop_contents();
  }

  return tx.ack;
}

/* ---- API: init --------------------------------------------------------- */

bool cec_transceiver_init(uint cec_gpio_arg, uint ddc_scl_gpio_arg,
                           uint ddc_sda_gpio_arg, uint hpd_gpio_arg) {
  cec_gpio = cec_gpio_arg;
  ddc_scl_gpio = ddc_scl_gpio_arg;
  ddc_sda_gpio = ddc_sda_gpio_arg;
  hpd_gpio = hpd_gpio_arg;

  // CEC pin: input with weak internal pull-up. The external 10k to 3.3V
  // is the primary pull-up; the internal (~50k) acts as a safety net if
  // the TV cuts its own pull-up when entering standby (some TVs power off
  // their HDMI section in standby, leaving the bus floating otherwise).
  gpio_init(cec_gpio);
  gpio_pull_up(cec_gpio);
  gpio_set_dir(cec_gpio, GPIO_IN);

  // HPD pin (optional)
  if (hpd_gpio != CEC_HPD_NOT_WIRED) {
    gpio_init(hpd_gpio);
    gpio_set_dir(hpd_gpio, GPIO_IN);
    gpio_pull_down(hpd_gpio);
  }

  // Register RX ISR
  gpio_set_irq_callback(&cec_rx_isr);
  irq_set_enabled(IO_IRQ_BANK0, true);

  // Start listening for CEC frames
  cec_rx_start();

  return true;
}

/* ---- API: physical address discovery (DDC/EDID) ------------------------ */

static const uint8_t edid_header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
static const uint8_t ctahdr[2] = {0x02, 0x03};
static const uint8_t vsbhdr[3] = {0x03, 0x0c, 0x00};

static uint16_t find_physical_address_in_block(const uint8_t *block, size_t len) {
  if (len < 4) return 0x0000;
  if (memcmp(&block[1], vsbhdr, 3) == 0) {
    return (block[4] << 8) | block[3];
  }
  return 0x0000;
}

static bool read_edid_block(i2c_inst_t *i2c, uint8_t offset, uint8_t *edid) {
  for (int attempt = 0; attempt < EDID_I2C_RETRIES; attempt++) {
    uint8_t ptr = offset;
    int ret = i2c_write_timeout_us(i2c, EDID_I2C_ADDR, &ptr, 1, true,
                                   EDID_I2C_TIMEOUT_US);
    if (ret != 1) {
      ret = i2c_write_timeout_us(i2c, EDID_I2C_ADDR, &ptr, 1, false,
                                 EDID_I2C_TIMEOUT_US);
    }

    if (ret != 1) {
      /* Some sinks NACK the offset write for block 0 but still answer a
       * plain sequential read starting at byte 0. Try that path before
       * giving up entirely. */
      if (offset == 0x00) {
        ret = i2c_read_timeout_us(i2c, EDID_I2C_ADDR, edid, EDID_BLOCK_SIZE,
                                  false, EDID_I2C_TIMEOUT_US);
        if (ret == EDID_BLOCK_SIZE) {
          uint16_t cksum = 0;
          for (size_t i = 0; i < EDID_BLOCK_SIZE; i++) cksum += edid[i];
          if ((cksum & 0x00ff) == 0x00) {
            return true;
          }
          last_discovery_error = "bad_checksum";
        } else {
          last_discovery_error = "i2c_read_timeout";
        }
      } else {
        last_discovery_error = "i2c_set_offset_failed";
      }
      sleep_ms(10);
      continue;
    }

    ret = i2c_read_timeout_us(i2c, EDID_I2C_ADDR, edid, EDID_BLOCK_SIZE,
                              false, EDID_I2C_TIMEOUT_US);
    if (ret != EDID_BLOCK_SIZE) {
      last_discovery_error = "i2c_read_timeout";
      sleep_ms(10);
      continue;
    }

    uint16_t cksum = 0;
    for (size_t i = 0; i < EDID_BLOCK_SIZE; i++) cksum += edid[i];
    if ((cksum & 0x00ff) != 0x00) {
      last_discovery_error = "bad_checksum";
      sleep_ms(10);
      continue;
    }

    return true;
  }

  return false;
}

static uint16_t edid_get_physical_address(i2c_inst_t *i2c) {
  uint8_t edid[EDID_I2C_READ_SIZE] = {0};

  if (!read_edid_block(i2c, 0x00, edid)) return 0x0000;
  if (memcmp(edid, edid_header, 8) != 0) {
    last_discovery_error = "bad_header";
    return 0x0000;
  }
  if (edid[126] == 0x00) {
    last_discovery_error = "no_cta_extension";
    return 0x0000;
  }

  if (!read_edid_block(i2c, 0x80, &edid[EDID_BLOCK_SIZE])) return 0x0000;

  uint8_t *cta = &edid[EDID_BLOCK_SIZE];
  if (memcmp(cta, ctahdr, 2) != 0) {
    last_discovery_error = "bad_cta_header";
    return 0x0000;
  }

  for (uint8_t i = EDID_CTA_DBC_OFFSET; i < cta[EDID_CTA_DTD_START];) {
    uint8_t *db = &cta[i];
    uint8_t len = db[0] & 0x1f;
    if (len == 0) { i++; continue; }
    uint16_t addr = find_physical_address_in_block(db, len);
    if (addr != 0x0000) return addr;
    i += len + 1;
  }

  last_discovery_error = "no_hdmi_vsdb";
  return 0x0000;
}

cec_physical_address_t cec_discover_physical_address(void) {
  i2c_inst_t *i2c = i2c0;
  last_discovery_error = "ok";

  /* Unstick the I2C bus: if SDA is stuck low (e.g. from a previous
   * interrupted transaction), bit-bang up to 9 SCL pulses until SDA
   * goes high, then send a STOP condition. This must be done before
   * handing the pins to the I2C peripheral. */
  gpio_init(ddc_scl_gpio);
  gpio_init(ddc_sda_gpio);
  gpio_set_dir(ddc_scl_gpio, GPIO_OUT);
  gpio_set_dir(ddc_sda_gpio, GPIO_IN);
  gpio_pull_up(ddc_scl_gpio);
  gpio_pull_up(ddc_sda_gpio);
  for (int i = 0; i < 9; i++) {
    gpio_put(ddc_scl_gpio, 0); sleep_ms(1);
    gpio_put(ddc_scl_gpio, 1); sleep_ms(1);
    if (gpio_get(ddc_sda_gpio)) break;
  }
  /* STOP condition: SDA low → SCL high → SDA high */
  gpio_set_dir(ddc_sda_gpio, GPIO_OUT);
  gpio_put(ddc_sda_gpio, 0); sleep_ms(1);
  gpio_put(ddc_scl_gpio, 1); sleep_ms(1);
  gpio_put(ddc_sda_gpio, 1); sleep_ms(1);

  /* Now hand pins to I2C peripheral */
  i2c_init(i2c, I2C_MASTER_FREQUENCY);
  gpio_set_function(ddc_scl_gpio, GPIO_FUNC_I2C);
  gpio_set_function(ddc_sda_gpio, GPIO_FUNC_I2C);
  gpio_pull_up(ddc_scl_gpio);
  gpio_pull_up(ddc_sda_gpio);
  sleep_ms(10);

  uint16_t pa = edid_get_physical_address(i2c);

  /* Release DDC pins back to high-impedance so the GPU can talk to the
   * TV's EDID ROM. Do NOT call i2c_deinit -- it corrupts the peripheral
   * state and prevents reinit from working on subsequent calls. */
  gpio_set_function(ddc_scl_gpio, GPIO_FUNC_SIO);
  gpio_set_function(ddc_sda_gpio, GPIO_FUNC_SIO);
  gpio_set_dir(ddc_scl_gpio, GPIO_IN);
  gpio_set_dir(ddc_sda_gpio, GPIO_IN);
  gpio_disable_pulls(ddc_scl_gpio);
  gpio_disable_pulls(ddc_sda_gpio);

  if (pa == 0x0000) {
    if (strcmp(last_discovery_error, "ok") == 0) {
      last_discovery_error = "pa_not_found";
    }
    return CEC_PA_UNKNOWN;
  }

  last_discovery_error = "ok";
  return pa;
}

const char *cec_last_discovery_error(void) {
  return last_discovery_error;
}

/* ---- API: logical address claiming ------------------------------------- */

bool cec_claim_logical_address(cec_physical_address_t my_pa,
                                uint8_t *out_logical_addr) {
  (void)my_pa;

  // We claim as a Playback Device (type 4). Probe the reserved addresses
  // in priority order and take the first unused one.
  static const uint8_t candidates[] = {0x04, 0x08, 0x0b, 0x0f};
  uint8_t addr = 0x0f;

  for (unsigned i = 0; i < sizeof(candidates); i++) {
    addr = candidates[i];
    if (addr == 0x0f) break;  // always available (unregistered)

    // Ping: send a poll message (header only, initiator == destination)
    uint8_t poll_msg = HEADER0(addr, addr);
    bool in_use = cec_frame_send_raw(&poll_msg, 1);
    if (!in_use) break;
  }

  *out_logical_addr = addr;
  rx.address = addr;  // ISR needs this to ACK frames addressed to us
  return true;
}

/* ---- API: transmit with retries ---------------------------------------- */

cec_tx_result_t cec_transmit(const cec_frame_t *frame, uint8_t max_retries) {
  uint8_t msg[CEC_FRAME_MAX_LEN];
  uint8_t len;

  msg[0] = HEADER0(frame->initiator, frame->destination);
  if (frame->param_len == 0 && frame->opcode == 0) {
    // Poll message
    len = 1;
  } else {
    msg[1] = frame->opcode;
    memcpy(&msg[2], frame->params, frame->param_len);
    len = 2 + frame->param_len;
  }

  for (uint8_t attempt = 0; attempt <= max_retries; attempt++) {
    if (attempt > 0) {
      busy_wait_us(2400 * 3);  // spec backoff: 3 bit-times
    }

    if (cec_frame_send_raw(msg, len)) {
      return CEC_TX_OK;
    }
  }

  return CEC_TX_NACK;
}

/* ---- API: receive poll -------------------------------------------------- */

bool cec_receive_poll(cec_frame_t *out_frame) {
  bool got = false;

  // Critical section: ISR must not fire between checking the flag and
  // re-arming (which clears the buffer). Also, after a TX operation the
  // RX IRQ is left disabled, so we always re-arm here.
  uint32_t irq_save = save_and_disable_interrupts();

  if (rx.frame_ready) {
    uint8_t header = rx.data[0];
    out_frame->initiator = (header >> 4) & 0x0f;
    out_frame->destination = header & 0x0f;

    if (rx.len >= 2) {
      out_frame->opcode = rx.data[1];
      uint8_t plen = rx.len - 2;
      if (plen > CEC_MAX_PAYLOAD) plen = CEC_MAX_PAYLOAD;
      memcpy(out_frame->params, &rx.data[2], plen);
      out_frame->param_len = plen;
    } else {
      out_frame->opcode = 0;
      out_frame->param_len = 0;
    }

    rx.frame_ready = false;
    got = true;
  }

  cec_rx_start();

  restore_interrupts(irq_save);

  return got;
}

/* ---- API: HPD ---------------------------------------------------------- */

bool cec_hpd_asserted(void) {
  if (hpd_gpio == CEC_HPD_NOT_WIRED) return true;
  return gpio_get(hpd_gpio);
}
