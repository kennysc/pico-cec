/*
 * cec_transceiver.h
 *
 * THIS FILE DEFINES THE INTERFACE, NOT THE IMPLEMENTATION.
 *
 * The functions declared here are expected to be implemented by adapting
 * gkoh/pico-cec's CEC frame send/receive state machines (edge-IRQ driven
 * receive, alarm-IRQ driven send, per the upstream README) plus its EDID/
 * DDC physical-address discovery code.
 *
 * I have not been able to read pico-cec's actual cec.c/cec.h/ddc.c source
 * in this session (GitHub blocks robot access to file views, and I only
 * have the README's architecture description, not the code). So this
 * header is a deliberately-designed SEAM: once you paste in the real
 * source files, the job is to implement *this* interface using their
 * internals, rather than inventing a parallel firmware from scratch.
 *
 * What's confirmed from upstream's README and should inform the real
 * implementation:
 *   - CEC RX: edge-interrupt driven state machine (not busy-wait, not PIO
 *     as of v0.7.0 -- PIO is listed as a future item, unimplemented)
 *   - CEC TX: alarm-interrupt driven state machine
 *   - EDID parsing already exists upstream to determine physical address
 *     ("EDID parsing to determine HDMI physical address" - confirmed
 *     working against real TVs and through an AVR, yielding different
 *     correct addresses depending on topology)
 *   - Passes the v4l-utils cec-compliance test suite as of v0.2.2+
 *   - GPIO direction-switching is used for open-drain CEC bus drive (no
 *     dedicated open-drain peripheral), consistent with how this header's
 *     functions should be implemented underneath
 */

#ifndef CEC_TRANSCEIVER_H
#define CEC_TRANSCEIVER_H

#include <stdint.h>
#include <stdbool.h>

#define CEC_MAX_PAYLOAD 16  // CEC frames: up to 1 header + 14 data bytes, +headroom

typedef struct {
    uint8_t initiator;          // logical address, 4 bits used
    uint8_t destination;        // logical address, 4 bits used (0xF = broadcast)
    uint8_t opcode;             // CEC opcode, e.g. CEC_OPCODE_STANDBY
    uint8_t params[CEC_MAX_PAYLOAD];
    uint8_t param_len;
} cec_frame_t;

typedef enum {
    CEC_TX_OK = 0,
    CEC_TX_NACK,        // bus-level NACK from destination (or broadcast: no listener cared)
    CEC_TX_ARB_LOST,    // lost arbitration, should retry per CEC spec backoff
    CEC_TX_TIMEOUT,
    CEC_TX_ERR,
} cec_tx_result_t;

/* Physical address, e.g. 1.0.0.0 packed as a single uint16_t (4 nibbles). */
typedef uint16_t cec_physical_address_t;
#define CEC_PA_UNKNOWN 0xFFFF

/*
 * --- Lifecycle -------------------------------------------------------
 */

// Bring up PIO/IRQ machinery, GPIO config for CEC pin, I2C0 for DDC.
// hpd_gpio is optional: pass a valid GPIO if HPD is wired (see wiring
// doc), or pass a sentinel value (e.g. 0xFF / CEC_HPD_NOT_WIRED) if not.
// When not wired, cec_hpd_asserted() should always return true so callers
// degrade safely without special-casing it themselves.
// Returns false on hardware init failure.
#define CEC_HPD_NOT_WIRED 0xFFu
bool cec_transceiver_init(uint cec_gpio, uint ddc_scl_gpio, uint ddc_sda_gpio,
                           uint hpd_gpio);

/*
 * --- Physical address discovery (DDC/EDID) ----------------------------
 *
 * Reads EDID over DDC (I2C master, address 0x50) from whatever HDMI port
 * this adapter is currently inline with, locates the CEA extension block,
 * finds the HDMI Vendor-Specific Data Block (IEEE OUI 00-0C-03), and
 * extracts the 16-bit physical address field.
 *
 * This is what makes the adapter port-agnostic: call it fresh after any
 * HPD transition (cable moved, TV power cycled) rather than caching a
 * value across sessions, since the correct address is a property of
 * *which physical port* the cable is in, not a fixed adapter setting.
 *
 * Returns CEC_PA_UNKNOWN on read/parse failure (e.g. TV not responding
 * on DDC yet, EDID malformed, no HDMI VSDB present).
 */
cec_physical_address_t cec_discover_physical_address(void);

/* Human-readable reason for the most recent cec_discover_physical_address()
 * failure. Returns "ok" after a successful discovery. */
const char *cec_last_discovery_error(void);

/*
 * --- Logical address claiming ------------------------------------------
 *
 * Per CEC spec, a device must claim a logical address (e.g. "Playback
 * Device 1") by polling for it being unused on the bus, before it can
 * participate normally. This wraps that negotiation.
 */
bool cec_claim_logical_address(cec_physical_address_t my_pa, uint8_t *out_logical_addr);

/*
 * --- TX with arbitration + ACK + retry --------------------------------
 *
 * Sends one frame. Internally must:
 *   - wait for bus idle (bus-free time, 3x nominal bit period after last
 *     activity per spec, longer if this is a retry)
 *   - drive start bit + header/data blocks with correct timing
 *   - detect arbitration loss (another initiator drove the bus low when
 *     we expected high) and back off
 *   - sample the ACK bit after each byte (destination pulls low to ACK,
 *     except broadcast frames which use inverted logic)
 *   - retry up to max_retries on NACK/ARB_LOST with spec-correct backoff
 *
 * This is the function that most needs to come from the real ported
 * pico-cec alarm-IRQ TX state machine -- arbitration/ACK/retry correctness
 * is exactly the part that's hard to get right from scratch and exactly
 * what cec-compliance validates.
 */
cec_tx_result_t cec_transmit(const cec_frame_t *frame, uint8_t max_retries);

/*
 * --- RX ------------------------------------------------------------------
 *
 * Non-blocking poll: returns true and fills *out_frame if a complete,
 * validated (correct EOM/ACK handling on our end) frame arrived since the
 * last call. Backed by the edge-IRQ receive state machine.
 */
bool cec_receive_poll(cec_frame_t *out_frame);

/*
 * --- HPD (optional) -------------------------------------------------------
 *
 * If HPD is wired (see wiring doc), this reports current hotplug state
 * and can be used to know when to re-run cec_discover_physical_address().
 * If HPD is not wired, always returns true and physical address should
 * instead be re-discovered periodically or on CEC bus errors.
 */
bool cec_hpd_asserted(void);

#endif // CEC_TRANSCEIVER_H
