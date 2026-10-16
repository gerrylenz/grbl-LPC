#include "net.h"
#include "net_config.h"
#include "emac.h"
#include "grbl.h"
#include "system_LPC17xx.h"

// One raw TCP session on port 23. No Telnet negotiation: LightBurn writes the
// same bytes it writes to a serial port, including 0x18 for soft reset.

static const uint8_t our_mac[6] = {
  GRBL_NET_MAC0, GRBL_NET_MAC1, GRBL_NET_MAC2,
  GRBL_NET_MAC3, GRBL_NET_MAC4, GRBL_NET_MAC5
};
static const uint8_t our_ip[4] = {
  GRBL_NET_IP_A, GRBL_NET_IP_B, GRBL_NET_IP_C, GRBL_NET_IP_D
};

enum TcpState {
  TCP_LISTEN = 0,
  TCP_SYN_RCVD,
  TCP_ESTABLISHED
};

static const int TXQ = 2048;
static uint8_t txq[TXQ] __attribute__((section(".eth_ram"), aligned(4)));
static uint16_t tx_tail = 0;
static uint16_t tx_sent = 0;
static uint16_t tx_head = 0;

static TcpState tcp_state = TCP_LISTEN;
static bool nic_up = false;
static bool link_up = false;
static bool in_poll = false;
static bool need_ack = false;
static bool syn_on_wire = false;
static uint16_t last_window = 0xFFFFu;

static uint8_t peer_mac[6];
static uint8_t peer_ip[4];
static uint16_t peer_port = 0;
static uint16_t peer_mss = 536;
static uint16_t peer_wnd = 0;

static uint32_t iss = 0;
static uint32_t snd_una = 0;
static uint32_t snd_nxt = 0;
static uint16_t inflight = 0;
static uint32_t rcv_nxt = 0;
static uint32_t tx_stamp = 0;
static uint32_t link_stamp = 0;
static uint8_t link_down_count = 0;
static uint16_t ip_id = 1;

static const uint16_t ETH_IP = 0x0800;
static const uint16_t ETH_ARP = 0x0806;
static const uint8_t IP_TCP = 6;
static const uint8_t IP_ICMP = 1;
static const uint8_t TCP_FIN = 0x01;
static const uint8_t TCP_SYN = 0x02;
static const uint8_t TCP_RST = 0x04;
static const uint8_t TCP_PSH = 0x08;
static const uint8_t TCP_ACK = 0x10;

static uint16_t rd16(const uint8_t *p)
{
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
  return ((uint32_t)rd16(p) << 16) | rd16(p + 2);
}

static void wr16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
  wr16(p, (uint16_t)(v >> 16));
  wr16(p + 2, (uint16_t)v);
}

static uint32_t csum_add(uint32_t sum, const uint8_t *p, int len)
{
  while (len > 1) {
    sum += (uint32_t)((p[0] << 8) | p[1]);
    p += 2;
    len -= 2;
  }
  if (len > 0) {
    sum += (uint32_t)(p[0] << 8);
  }
  return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
  while (sum >> 16) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
  }
  return (uint16_t)~sum;
}

static uint32_t ticks_from_ms(uint32_t ms)
{
  return (uint32_t)((uint64_t)SystemCoreClock * ms / 1000u);
}

static uint16_t q_count(uint16_t from, uint16_t to)
{
  return (to >= from) ? (uint16_t)(to - from) : (uint16_t)(TXQ - from + to);
}

static uint16_t q_free(void)
{
  return (uint16_t)(TXQ - 1 - q_count(tx_tail, tx_head));
}

static uint16_t advertised_window(void)
{
  uint16_t free = serial_get_rx_buffer_available();
  if (free > 1024) {
    free = 1024;
  }
  return free;
}

static bool buffered_byte(uint8_t b)
{
  return b != CMD_RESET && b != CMD_STATUS_REPORT && b != CMD_CYCLE_START &&
         b != CMD_FEED_HOLD && b <= 0x7F;
}

static void copy_mac(uint8_t *dst, const uint8_t *src)
{
  for (int i = 0; i < 6; i++) {
    dst[i] = src[i];
  }
}

static bool same_ip(const uint8_t *a, const uint8_t *b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static bool mac_for_us(const uint8_t *dst)
{
  bool bcast = true;
  bool ours = true;
  for (int i = 0; i < 6; i++) {
    if (dst[i] != 0xFF) {
      bcast = false;
    }
    if (dst[i] != our_mac[i]) {
      ours = false;
    }
  }
  return bcast || ours;
}

static void queue_clear(void)
{
  tx_tail = tx_sent = tx_head = 0;
  inflight = 0;
  need_ack = false;
  syn_on_wire = false;
  last_window = 0xFFFFu;
}

static void tcp_drop(bool stop_motion);

static int write_ip(uint8_t *f, const uint8_t *dst_mac, const uint8_t *dst_ip,
                      int ip_payload, uint8_t proto)
{
  copy_mac(f, dst_mac);
  copy_mac(f + 6, our_mac);
  wr16(f + 12, ETH_IP);
  uint8_t *ip = f + 14;
  int total = 20 + ip_payload;
  ip[0] = 0x45;
  ip[1] = 0;
  wr16(ip + 2, (uint16_t)total);
  wr16(ip + 4, ip_id++);
  wr16(ip + 6, 0x4000);
  ip[8] = 64;
  ip[9] = proto;
  wr16(ip + 10, 0);
  for (int i = 0; i < 4; i++) {
    ip[12 + i] = our_ip[i];
    ip[16 + i] = dst_ip[i];
  }
  wr16(ip + 10, csum_fold(csum_add(0, ip, 20)));
  return 34;
}

static bool send_frame(int len)
{
  return emac_tx_commit(len);
}

static void send_arp_reply(const uint8_t *src_mac, const uint8_t *src_ip)
{
  uint8_t *f = emac_tx_buffer();
  if (!f) {
    return;
  }
  copy_mac(f, src_mac);
  copy_mac(f + 6, our_mac);
  wr16(f + 12, ETH_ARP);
  uint8_t *a = f + 14;
  wr16(a, 1);
  wr16(a + 2, ETH_IP);
  a[4] = 6;
  a[5] = 4;
  wr16(a + 6, 2);
  copy_mac(a + 8, our_mac);
  for (int i = 0; i < 4; i++) {
    a[14 + i] = our_ip[i];
    a[24 + i] = src_ip[i];
  }
  copy_mac(a + 18, src_mac);
  send_frame(42);
}

static void send_icmp_echo(const uint8_t *ip, int ip_len, const uint8_t *src_mac)
{
  if (ip_len < 28 || ip[9] != IP_ICMP || ip[20] != 8) {
    return;
  }
  uint8_t *f = emac_tx_buffer();
  if (!f) {
    return;
  }
  int payload = ip_len - 20;
  write_ip(f, src_mac, ip + 12, payload, IP_ICMP);
  uint8_t *icmp = f + 34;
  for (int i = 0; i < payload; i++) {
    icmp[i] = ip[20 + i];
  }
  icmp[0] = 0;
  wr16(icmp + 2, 0);
  wr16(icmp + 2, csum_fold(csum_add(0, icmp, payload)));
  send_frame(34 + payload);
}

static bool send_tcp(uint8_t flags, const uint8_t *payload, int len, uint32_t seq, bool mss)
{
  uint8_t *f = emac_tx_buffer();
  if (!f) {
    return false;
  }
  int hdr = mss ? 24 : 20;
  int off = write_ip(f, peer_mac, peer_ip, hdr + len, IP_TCP);
  uint8_t *t = f + off;
  wr16(t, GRBL_NET_PORT);
  wr16(t + 2, peer_port);
  wr32(t + 4, seq);
  wr32(t + 8, rcv_nxt);
  t[12] = (uint8_t)((hdr / 4) << 4);
  t[13] = flags;
  uint16_t wnd = advertised_window();
  wr16(t + 14, wnd);
  wr16(t + 16, 0);
  wr16(t + 18, 0);
  if (mss) {
    t[20] = 2;
    t[21] = 4;
    wr16(t + 22, GRBL_NET_MSS);
  }
  if (len > 0 && payload) {
    for (int i = 0; i < len; i++) {
      t[hdr + i] = payload[i];
    }
  }
  uint32_t sum = 0;
  sum += (uint32_t)rd16(our_ip);
  sum += (uint32_t)rd16(our_ip + 2);
  sum += (uint32_t)rd16(peer_ip);
  sum += (uint32_t)rd16(peer_ip + 2);
  sum += IP_TCP;
  sum += (uint32_t)(hdr + len);
  sum = csum_add(sum, t, hdr + len);
  wr16(t + 16, csum_fold(sum));
  if (!send_frame(off + hdr + len)) {
    return false;
  }
  last_window = wnd;
  if (flags & TCP_ACK) {
    need_ack = false;
  }
  return true;
}

static void copy_out(uint8_t *dst, uint16_t index, int len)
{
  for (int i = 0; i < len; i++) {
    dst[i] = txq[(uint16_t)(index + i) % TXQ];
  }
}

static void tcp_output(void)
{
  if (!link_up) {
    return;
  }
  uint32_t now = get_time();
  if (tcp_state == TCP_SYN_RCVD) {
    if (!syn_on_wire || (now - tx_stamp) >= ticks_from_ms(500)) {
      if (send_tcp(TCP_SYN | TCP_ACK, 0, 0, iss, true)) {
        snd_nxt = iss + 1u;
        syn_on_wire = true;
        tx_stamp = now;
      }
    }
    return;
  }
  if (tcp_state != TCP_ESTABLISHED) {
    return;
  }

  static uint8_t seg[GRBL_NET_MSS];
  if (inflight > 0 && inflight <= GRBL_NET_MSS && (now - tx_stamp) >= ticks_from_ms(200)) {
    copy_out(seg, tx_tail, inflight);
    if (send_tcp(TCP_ACK | TCP_PSH, seg, inflight, snd_una, false)) {
      tx_stamp = now;
    }
    return;
  }

  uint16_t unsent = q_count(tx_sent, tx_head);
  if (inflight == 0 && unsent > 0 && peer_wnd > 0) {
    int n = unsent;
    if (n > GRBL_NET_MSS) {
      n = GRBL_NET_MSS;
    }
    if (n > peer_mss) {
      n = peer_mss;
    }
    if (n > peer_wnd) {
      n = peer_wnd;
    }
    copy_out(seg, tx_sent, n);
    if (send_tcp(TCP_ACK | TCP_PSH, seg, n, snd_nxt, false)) {
      tx_sent = (uint16_t)((tx_sent + n) % TXQ);
      snd_nxt += (uint32_t)n;
      inflight = (uint16_t)n;
      tx_stamp = now;
    }
    return;
  }

  uint16_t wnd = advertised_window();
  if (need_ack || wnd != last_window) {
    send_tcp(TCP_ACK, 0, 0, snd_nxt, false);
  }
}

static void tcp_drop(bool stop_motion)
{
  bool was_up = tcp_state == TCP_ESTABLISHED;
  tcp_state = TCP_LISTEN;
  queue_clear();
  peer_port = 0;
  if (was_up && stop_motion) {
    serial_reset_read_buffer();
    mc_reset();
  }
}

static void accept_payload(const uint8_t *data, int len, uint32_t seq)
{
  int32_t off = (int32_t)(seq - rcv_nxt);
  if (off > 0) {
    need_ack = true;
    return;
  }
  if (off < 0) {
    int skip = -off;
    if (skip >= len) {
      need_ack = true;
      return;
    }
    data += skip;
    len -= skip;
  }

  uint16_t free = serial_get_rx_buffer_available();
  uint16_t used = 0;
  int accept = 0;
  for (int i = 0; i < len; i++) {
    if (buffered_byte(data[i])) {
      if (used >= free) {
        break;
      }
      used++;
    }
    accept++;
  }
  for (int i = 0; i < accept; i++) {
    serial_rx_byte(data[i]);
  }
  rcv_nxt += (uint32_t)accept;
  need_ack = true;
}

static void take_ack(uint32_t ack)
{
  if (tcp_state == TCP_SYN_RCVD) {
    if (ack == iss + 1u) {
      tcp_state = TCP_ESTABLISHED;
      snd_una = ack;
      snd_nxt = ack;
      inflight = 0;
      tx_tail = tx_sent = tx_head = 0;
    }
    return;
  }
  int32_t advanced = (int32_t)(ack - snd_una);
  if (advanced <= 0 || (uint32_t)advanced > inflight) {
    return;
  }
  tx_tail = (uint16_t)((tx_tail + advanced) % TXQ);
  snd_una = ack;
  inflight = (uint16_t)(inflight - advanced);
}

static uint16_t parse_mss(const uint8_t *tcp, int hdr_len)
{
  int i = 20;
  while (i + 1 < hdr_len) {
    uint8_t kind = tcp[i];
    if (kind == 0) {
      break;
    }
    if (kind == 1) {
      i++;
      continue;
    }
    uint8_t opt_len = tcp[i + 1];
    if (opt_len < 2 || i + opt_len > hdr_len) {
      break;
    }
    if (kind == 2 && opt_len == 4) {
      return rd16(tcp + i + 2);
    }
    i += opt_len;
  }
  return 536;
}

static void handle_tcp(const uint8_t *ip, int ip_len, const uint8_t *src_mac)
{
  int ihl = (ip[0] & 0x0F) * 4;
  if (ihl < 20 || ip_len < ihl + 20) {
    return;
  }
  const uint8_t *t = ip + ihl;
  int tcp_len = ip_len - ihl;
  int hdr_len = ((t[12] >> 4) & 0x0F) * 4;
  if (hdr_len < 20 || hdr_len > tcp_len) {
    return;
  }
  if (rd16(t + 2) != GRBL_NET_PORT) {
    return;
  }

  uint32_t sum = 0;
  sum += (uint32_t)rd16(ip + 12);
  sum += (uint32_t)rd16(ip + 14);
  sum += (uint32_t)rd16(ip + 16);
  sum += (uint32_t)rd16(ip + 18);
  sum += IP_TCP;
  sum += (uint32_t)tcp_len;
  if (csum_fold(csum_add(sum, t, tcp_len)) != 0) {
    return;
  }

  uint8_t flags = t[13];
  uint32_t seq = rd32(t + 4);
  uint32_t ack = rd32(t + 8);
  const uint8_t *payload = t + hdr_len;
  int plen = tcp_len - hdr_len;

  if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
    if (tcp_state == TCP_ESTABLISHED) {
      tcp_drop(true);
    }
    copy_mac(peer_mac, src_mac);
    for (int i = 0; i < 4; i++) {
      peer_ip[i] = ip[12 + i];
    }
    peer_port = rd16(t);
    peer_mss = parse_mss(t, hdr_len);
    if (peer_mss < 1) {
      peer_mss = 536;
    }
    peer_wnd = rd16(t + 14);
    iss = get_time() | 1u;
    rcv_nxt = seq + 1u;
    snd_una = iss;
    snd_nxt = iss;
    queue_clear();
    tcp_state = TCP_SYN_RCVD;
    syn_on_wire = false;
    return;
  }

  if (tcp_state == TCP_LISTEN || peer_port != rd16(t) || !same_ip(peer_ip, ip + 12)) {
    return;
  }

  if (flags & TCP_RST) {
    tcp_drop(true);
    return;
  }
  if (flags & TCP_ACK) {
    peer_wnd = rd16(t + 14);
    take_ack(ack);
  }
  if (tcp_state == TCP_ESTABLISHED && plen > 0) {
    accept_payload(payload, plen, seq);
  }
  if ((flags & TCP_FIN) && tcp_state == TCP_ESTABLISHED && seq + (uint32_t)plen == rcv_nxt) {
    rcv_nxt += 1u;
    send_tcp(TCP_FIN | TCP_ACK, 0, 0, snd_nxt, false);
    tcp_drop(true);
  }
}

static bool ip_checksum_ok(const uint8_t *ip, int ihl)
{
  return csum_fold(csum_add(0, ip, ihl)) == 0;
}

static void handle_frame(uint8_t *frame, int len)
{
  if (len < 14 || !mac_for_us(frame)) {
    return;
  }
  uint16_t kind = rd16(frame + 12);
  if (kind == ETH_ARP) {
    if (len < 42) {
      return;
    }
    const uint8_t *a = frame + 14;
    if (rd16(a) != 1 || rd16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4) {
      return;
    }
    if (rd16(a + 6) == 1 && same_ip(a + 24, our_ip)) {
      send_arp_reply(a + 8, a + 14);
    }
    return;
  }
  if (kind != ETH_IP || len < 34) {
    return;
  }
  uint8_t *ip = frame + 14;
  if ((ip[0] >> 4) != 4) {
    return;
  }
  int ihl = (ip[0] & 0x0F) * 4;
  int total = rd16(ip + 2);
  if (ihl < 20 || total < ihl || len < 14 + total) {
    return;
  }
  if (!ip_checksum_ok(ip, ihl) || !same_ip(ip + 16, our_ip)) {
    return;
  }
  if ((rd16(ip + 6) & 0x3FFF) != 0) {
    return;
  }
  if (ip[9] == IP_ICMP) {
    send_icmp_echo(ip, total, frame + 6);
    return;
  }
  if (ip[9] == IP_TCP) {
    handle_tcp(ip, total, frame + 6);
  }
}

static void service(void)
{
  if (!nic_up) {
    return;
  }
  uint32_t now = get_time();
  if (!link_up || (now - link_stamp) >= ticks_from_ms(500)) {
    link_stamp = now;
    bool up = emac_link_up();
    if (up) {
      link_down_count = 0;
      link_up = true;
    } else if (link_up && ++link_down_count >= 4) {
      link_up = false;
      link_down_count = 0;
      tcp_drop(true);
    }
  }

  for (int n = 0; n < 8; n++) {
    uint8_t *frame = 0;
    int len = emac_rx(&frame);
    if (len <= 0 || !frame) {
      break;
    }
    link_up = true;
    link_down_count = 0;
    handle_frame(frame, len);
    emac_rx_done();
    tcp_output();
  }
  tcp_output();
}

void net_init(void)
{
  nic_up = emac_init(our_mac);
  link_up = false;
  tcp_state = TCP_LISTEN;
  queue_clear();
  link_stamp = get_time();
}

void net_poll(void)
{
  if (in_poll) {
    return;
  }
  in_poll = true;
  service();
  in_poll = false;
}

void net_write(uint8_t data)
{
  if (tcp_state != TCP_ESTABLISHED) {
    return;
  }
  if (q_free() == 0) {
    if (in_poll) {
      return;
    }
    uint32_t start = get_time();
    while (q_free() == 0) {
      net_poll();
      if (sys_rt_exec_state & EXEC_RESET) {
        return;
      }
      if ((get_time() - start) > SystemCoreClock * 2u) {
        tcp_drop(true);
        return;
      }
    }
    if (tcp_state != TCP_ESTABLISHED) {
      return;
    }
  }
  txq[tx_head] = data;
  tx_head = (uint16_t)((tx_head + 1u) % TXQ);
}
