#ifndef emac_h
#define emac_h

#include <stdint.h>
#include <stdbool.h>

// LPC1769 EMAC + LAN8720A (Smoothieboard RMII). Packet memory is in AHB SRAM.

bool emac_init(const uint8_t mac[6]);

// Returns the frame length, or 0 when no frame is waiting.
// The pointer stays valid until emac_rx_done().
int emac_rx(uint8_t **frame);

void emac_rx_done(void);

// Pointer to the transmit buffer when the MAC is idle, otherwise 0.
uint8_t *emac_tx_buffer(void);

// Hand a frame of `len` bytes in emac_tx_buffer() to the MAC.
bool emac_tx_commit(int len);

// Updates link speed from the PHY. Call about twice a second.
bool emac_link_up(void);

#endif
