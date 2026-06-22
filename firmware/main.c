/*
 * main.c - pico-cec-bridge application layer
 *
 * Implements the PWR_ON / PWR_OFF / PING serial protocol on top of
 * cec_transceiver.h (see that header for what's adapted-from-upstream
 * vs to-be-implemented).
 *
 * Serial protocol (115200 8N1, newline terminated ASCII):
 *
 *   PC -> Pico:
 *     CMD:PWR_ON
 *     CMD:PWR_OFF
 *     CMD:PA           (query physical address)
 *     CMD:PING
 *
 *   Pico -> PC:
 *     ACK:<cmd>
 *     NACK:<cmd>:<reason>
 *     PONG
 *     EVT:CEC_TV_STANDBY          (TV sent <Standby> -- user used their remote)
 *     EVT:CEC_TV_ON                (TV reports <Report Power Status> = on)
 *     EVT:CEC_ACTIVE_SOURCE_LOST   (another device sent <Active Source>)
 *     EVT:READY:PA=<a.b.c.d>       (boot complete, physical address resolved)
 *     EVT:ERROR:<message>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "cec_transceiver.h"
#include "ws2812.h"

/* ---- CEC protocol constants (CEC 1.4b) -------------------------------- */

#define CEC_OPCODE_STANDBY              0x36
#define CEC_OPCODE_IMAGE_VIEW_ON        0x04
#define CEC_OPCODE_ACTIVE_SOURCE        0x82
#define CEC_OPCODE_REPORT_POWER_STATUS  0x90
#define CEC_OPCODE_GIVE_DEVICE_POWER_STATUS 0x8F

#define CEC_LOGICAL_ADDR_TV       0x0
#define CEC_LOGICAL_ADDR_BROADCAST 0xF

#define CEC_POWER_STATUS_ON       0x00
#define CEC_POWER_STATUS_STANDBY  0x01

/*
 * GPIO pin assignments -- board: Waveshare RP2040-Zero.
 *
 * GP2/GP3/GP4/GP5 are all present on the RP2040-Zero's 23-pin edge header
 * (board exposes GP0-GP29 generically; no conflicts with USB, the 2MB
 * QSPI flash, or the crystal, which use other dedicated pins not on this
 * list). Confirmed against Waveshare's own pinout reference.
 *
 * GP16 is NOT available for general use here: it's hardwired on-board to
 * the WS2812 (NeoPixel) status LED's DIN, confirmed via Waveshare's own
 * example firmware and multiple independent community sources. Don't
 * repurpose it for anything else; use it via PIN_STATUS_LED below if/when
 * the WS2812 status-light feature (pico-cec's blue/green/red convention)
 * gets implemented -- see WIRING_AND_SETUP.md, currently documented but
 * not yet wired into firmware logic.
 */
#define PIN_CEC          2
#define PIN_DDC_SCL      4
#define PIN_DDC_SDA      5
#define PIN_HPD          3   /* optional; wire via divider, see wiring doc */
#define PIN_STATUS_LED  16   /* onboard WS2812, RP2040-Zero only.
                                * Driven by ws2812.c (PIO-based WS2812
                                * driver). Colour conventions:
                                *   Blue  = booting / EDID discovery
                                *   Green = ready (normal operation)
                                *   Red   = fatal error
                                *   Yellow = TV standby detected */

#define MAX_TX_RETRIES 5

/* ---- Global state ------------------------------------------------------ */

static cec_physical_address_t g_my_pa = CEC_PA_UNKNOWN;
static uint8_t g_my_logical_addr = CEC_LOGICAL_ADDR_BROADCAST; /* invalid until claimed */

/* ---- Serial helpers ------------------------------------------------------ */

static void send_line(const char *s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void send_ack(const char *cmd) {
    char buf[64];
    snprintf(buf, sizeof(buf), "ACK:%s", cmd);
    send_line(buf);
}

static void send_nack(const char *cmd, const char *reason) {
    char buf[128];
    snprintf(buf, sizeof(buf), "NACK:%s:%s", cmd, reason);
    send_line(buf);
}

static void send_event(const char *evt) {
    char buf[128];
    snprintf(buf, sizeof(buf), "EVT:%s", evt);
    send_line(buf);
}

static void send_ready_event(cec_physical_address_t pa) {
    char buf[64];
    snprintf(buf, sizeof(buf), "EVT:READY:PA=%u.%u.%u.%u",
              (pa >> 12) & 0xF, (pa >> 8) & 0xF, (pa >> 4) & 0xF, pa & 0xF);
    send_line(buf);
}

/* ---- CEC actions -------------------------------------------------------- */

/*
 * Re-resolve physical address via DDC/EDID. Call at boot and whenever we
 * suspect topology changed (HPD transition, or before a PWR_ON if it's
 * been a while) -- this is the mechanism that makes the adapter
 * port-agnostic rather than hardcoded to "HDMI1" etc.
 */
static bool refresh_physical_address(void) {
    cec_physical_address_t pa = cec_discover_physical_address();
    if (pa == CEC_PA_UNKNOWN) {
        send_event("ERROR:edid_read_failed");
        return false;
    }
    g_my_pa = pa;
    send_ready_event(pa);
    ws2812_set_hex(WS2812_GREEN);

    uint8_t la;
    if (!cec_claim_logical_address(g_my_pa, &la)) {
        send_event("ERROR:logical_addr_claim_failed");
        ws2812_set_hex(WS2812_RED);
        return false;
    }
    g_my_logical_addr = la;
    return true;
}

static bool do_pwr_on(void) {
    if (g_my_pa == CEC_PA_UNKNOWN) {
        return false;
    }

    cec_frame_t f = {0};

    /* <Image View On> to TV: wakes the display out of standby. */
    f.initiator = g_my_logical_addr;
    f.destination = CEC_LOGICAL_ADDR_TV;
    f.opcode = CEC_OPCODE_IMAGE_VIEW_ON;
    f.param_len = 0;
    cec_tx_result_t r1 = cec_transmit(&f, MAX_TX_RETRIES);
    if (r1 != CEC_TX_OK) {
        return false;
    }

    /* <Active Source> broadcast carrying OUR physical address: this is
     * the message that makes the TV switch its input to wherever we're
     * actually plugged in -- no hardcoded port number anywhere, the
     * address itself came from EDID discovery above. */
    f.initiator = g_my_logical_addr;
    f.destination = CEC_LOGICAL_ADDR_BROADCAST;
    f.opcode = CEC_OPCODE_ACTIVE_SOURCE;
    f.params[0] = (g_my_pa >> 8) & 0xFF;
    f.params[1] = g_my_pa & 0xFF;
    f.param_len = 2;
    cec_tx_result_t r2 = cec_transmit(&f, MAX_TX_RETRIES);

    return r2 == CEC_TX_OK;
}

static bool do_pwr_off(void) {
    cec_frame_t f = {0};
    f.initiator = g_my_logical_addr;
    f.destination = CEC_LOGICAL_ADDR_BROADCAST;
    f.opcode = CEC_OPCODE_STANDBY;
    f.param_len = 0;
    return cec_transmit(&f, MAX_TX_RETRIES) == CEC_TX_OK;
}

/* ---- Incoming CEC frame handling (from the TV / other devices) --------- */

static void handle_incoming_frame(const cec_frame_t *f) {
    /* Only care about frames addressed to us or broadcast. */
    if (f->destination != g_my_logical_addr &&
        f->destination != CEC_LOGICAL_ADDR_BROADCAST) {
        return;
    }

    switch (f->opcode) {
        case CEC_OPCODE_STANDBY:
            /* TV (or anything) broadcasting standby -- if it came from the
             * TV specifically, that's our trigger to suspend the PC. */
            if (f->initiator == CEC_LOGICAL_ADDR_TV) {
                ws2812_set_hex(WS2812_YELLOW);
                send_event("CEC_TV_STANDBY");
            }
            break;

        case CEC_OPCODE_REPORT_POWER_STATUS:
            if (f->initiator == CEC_LOGICAL_ADDR_TV && f->param_len >= 1) {
                if (f->params[0] == CEC_POWER_STATUS_ON) {
                    ws2812_set_hex(WS2812_GREEN);
                    send_event("CEC_TV_ON");
                }
            }
            break;

        case CEC_OPCODE_ACTIVE_SOURCE:
            if (f->initiator != g_my_logical_addr) {
                send_event("CEC_ACTIVE_SOURCE_LOST");
            }
            break;

        case CEC_OPCODE_GIVE_DEVICE_POWER_STATUS: {
            /* Someone's asking us our power state -- always report "on"
             * since if we're answering, we're awake. */
            cec_frame_t reply = {0};
            reply.initiator = g_my_logical_addr;
            reply.destination = f->initiator;
            reply.opcode = CEC_OPCODE_REPORT_POWER_STATUS;
            reply.params[0] = CEC_POWER_STATUS_ON;
            reply.param_len = 1;
            cec_transmit(&reply, MAX_TX_RETRIES);
            break;
        }

        default:
            break;
    }
}

/* ---- Serial command parsing ---------------------------------------------- */

static void handle_command_line(char *line) {
    /* Expect "CMD:<NAME>" */
    if (strncmp(line, "CMD:", 4) != 0) {
        return;
    }
    const char *cmd = line + 4;

    if (strcmp(cmd, "PWR_ON") == 0) {
        if (do_pwr_on()) {
            send_ack("PWR_ON");
        } else {
            send_nack("PWR_ON", "cec_tx_failed");
        }
    } else if (strcmp(cmd, "PWR_OFF") == 0) {
        if (do_pwr_off()) {
            send_ack("PWR_OFF");
        } else {
            send_nack("PWR_OFF", "cec_tx_failed");
        }
    } else if (strcmp(cmd, "PA") == 0) {
        if (g_my_pa != CEC_PA_UNKNOWN) {
            send_ready_event(g_my_pa);
        } else {
            send_nack("PA", "pa_unknown");
        }
    } else if (strcmp(cmd, "PING") == 0) {
        send_line("PONG");
    } else {
        send_nack(cmd, "unknown_command");
    }
}

/* ---- Main ----------------------------------------------------------------- */

int main(void) {
    stdio_init_all();

    /* Wait for USB serial to enumerate so the host can connect picocom
     * before we attempt the EDID read. The LED pulses white briefly to
     * signal "connect now", then goes blue when the window closes. */
    ws2812_init(PIN_STATUS_LED);
    sleep_ms(10);
    ws2812_set_hex(WS2812_BLUE);   /* booting */

    if (!cec_transceiver_init(PIN_CEC, PIN_DDC_SCL, PIN_DDC_SDA, PIN_HPD)) {
        send_event("ERROR:transceiver_init_failed");
        ws2812_set_hex(WS2812_RED);
        while (1) {
            tight_loop_contents();
        }
    }

    /* Physical address is hardcoded -- DDC/EDID is not available because
     * the GPU uses a DP-to-HDMI adapter that blocks DDC, and the TV is
     * on HDMI port 1 giving physical address 1.0.0.0 = 0x1000.
     * If the TV port changes, update this value. */
    g_my_pa = 0x1000;
    send_ready_event(g_my_pa);
    ws2812_set_hex(WS2812_GREEN);
    uint8_t la;
    if (!cec_claim_logical_address(g_my_pa, &la)) {
        send_event("ERROR:logical_addr_claim_failed");
        ws2812_set_hex(WS2812_RED);
    } else {
        g_my_logical_addr = la;
    }

    /* Send <Image View On> + <Active Source> at boot so the TV wakes and
     * switches to this input autonomously -- like a PS5, no PC command
     * needed. */
    if (!do_pwr_on()) {
        send_event("AUTO_POWER_ON_FAILED");
    }

    /* If we made it this far without errors, show ready */
    ws2812_set_hex(WS2812_GREEN);

    char line_buf[128];
    size_t line_len = 0;

    while (true) {
        /* Drain any pending incoming CEC frames (non-blocking poll backed
         * by the edge-IRQ receive state machine). */
        cec_frame_t rx;
        while (cec_receive_poll(&rx)) {
            handle_incoming_frame(&rx);
        }

        /* If HPD is wired and indicates the link just came back (e.g. TV
         * was power-cycled or cable moved), invalidate our cached
         * physical address so the next PWR_ON re-discovers it rather than
         * trusting a possibly-stale value. */
        static bool last_hpd = true;
        bool hpd = cec_hpd_asserted();
        if (hpd && !last_hpd) {
            g_my_pa = CEC_PA_UNKNOWN;
        }
        last_hpd = hpd;

        /* Non-blocking-ish line read from USB serial. getchar_timeout_us
         * keeps this loop responsive to CEC RX above rather than blocking
         * on serial input. */
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    line_buf[line_len] = '\0';
                    handle_command_line(line_buf);
                    line_len = 0;
                }
            } else if (line_len < sizeof(line_buf) - 1) {
                line_buf[line_len++] = (char)c;
            }
        }
    }

    return 0;
}
