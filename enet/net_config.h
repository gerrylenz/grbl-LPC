#ifndef net_config_h
#define net_config_h

// LightBurn connects with device type GRBL, connection "Ethernet or Wifi/TCP".
// Port is 23 (LightBurn's default). Change the address if 222 is already used
// on the LAN. The PC and the board must be in this subnet.
// This machine's LAN is 192.168.178.0/24 (gateway 192.168.178.1).

#define GRBL_NET_IP_A 192
#define GRBL_NET_IP_B 168
#define GRBL_NET_IP_C 178
#define GRBL_NET_IP_D 222

#define GRBL_NET_PORT 23

// Locally administered MAC. Not routed; only used on this LAN.
#define GRBL_NET_MAC0 0x02
#define GRBL_NET_MAC1 0x47
#define GRBL_NET_MAC2 0x52
#define GRBL_NET_MAC3 0x42
#define GRBL_NET_MAC4 0x4C
#define GRBL_NET_MAC5 0x01

// Grbl RX buffer is 1024. Stay under that so a segment is never larger than
// the buffer that has to accept it.
#define GRBL_NET_MSS 512

#endif
