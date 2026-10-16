#ifndef net_h
#define net_h

#include <stdint.h>

// Raw TCP port 23. Bytes are the Grbl serial stream LightBurn already uses.

void net_init(void);
void net_poll(void);
void net_write(uint8_t data);

#endif
