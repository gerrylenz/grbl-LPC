#include "emac.h"
#include "LPC17xx.h"
#include "system_LPC17xx.h"

// Register bits from the LPC17xx EMAC manual. The MAC DMA can only reach
// AHB SRAM, so descriptors and frames live in the IRAM2 section.

#define MAC1_REC_EN     0x00000001u
#define MAC1_PASS_ALL   0x00000002u
#define MAC1_RES_TX     0x00000100u
#define MAC1_RES_MCS_TX 0x00000200u
#define MAC1_RES_RX     0x00000400u
#define MAC1_RES_MCS_RX 0x00000800u
#define MAC1_SIM_RES    0x00004000u
#define MAC1_SOFT_RES   0x00008000u

#define MAC2_FULL_DUP   0x00000001u
#define MAC2_CRC_EN     0x00000010u
#define MAC2_PAD_EN     0x00000020u

#define IPGT_FULL_DUP   0x15u
#define IPGT_HALF_DUP   0x12u
#define IPGR_DEF        0x00000C12u
#define CLRT_DEF        0x0000370Fu
#define MAX_FRAME_LEN   1536u

#define SUPP_SPEED      0x00000100u
#define SUPP_RES_RMII   0x00000800u

#define MCFG_CLK_SEL(n) (((n) & 0x0Fu) << 2)
#define MCFG_RES_MII    0x00008000u
#define MCMD_READ       0x00000001u
#define MIND_BUSY       0x00000001u

#define CR_RX_EN        0x00000001u
#define CR_TX_EN        0x00000002u
#define CR_REG_RES      0x00000008u
#define CR_TX_RES       0x00000010u
#define CR_RX_RES       0x00000020u
#define CR_PASS_RUNT    0x00000040u
#define CR_RMII         0x00000200u
#define CR_FULL_DUP     0x00000400u

#define RFC_UCAST_EN    0x00000001u
#define RFC_BCAST_EN    0x00000002u
#define RFC_PERFECT_EN  0x00000020u

#define RCTRL_INT       0x80000000u
#define RINFO_SIZE      0x000007FFu
#define RINFO_LAST      0x40000000u
#define RINFO_ERR       0x80000000u
#define TCTRL_PAD       0x10000000u
#define TCTRL_CRC       0x20000000u
#define TCTRL_LAST      0x40000000u
#define TCTRL_INT       0x80000000u

#define PHY_ADDR        0x0100u
#define PHY_BMCR        0x00u
#define PHY_BMSR        0x01u
#define PHY_IDR1        0x02u
#define PHY_IDR2        0x03u
#define PHY_SCSR        0x1Fu
#define PHY_BMCR_RESET  0x8000u
#define PHY_BMCR_AN     0x1000u
#define PHY_BMSR_AN_DONE 0x0020u
#define PHY_BMSR_LINK   0x0004u
#define PHY_ID_8720A    0x0007C0F0u

#define N_RX 4
#define N_TX 2

struct EthDesc {
  uint32_t packet;
  uint32_t control;
};

struct EthStat {
  uint32_t info;
  uint32_t extra;
};

struct EthRam {
  uint8_t rx_buf[N_RX][MAX_FRAME_LEN];
  uint8_t tx_buf[N_TX][MAX_FRAME_LEN];
  EthDesc rx_desc[N_RX];
  EthStat rx_stat[N_RX];
  EthDesc tx_desc[N_TX];
  EthStat tx_stat[N_TX];
};

static EthRam eth __attribute__((section(".eth_ram"), aligned(8), used));

static bool mac_ready = false;
static bool full_duplex = true;
static bool speed_100 = true;
static bool applied_full = false;
static bool applied_100 = false;
static int rx_index = 0;

static void mii_wait(void)
{
  for (uint32_t i = 0; i < 100000u; i++) {
    if ((LPC_EMAC->MIND & MIND_BUSY) == 0) {
      return;
    }
  }
}

static bool phy_write(uint32_t reg, uint16_t value)
{
  LPC_EMAC->MADR = PHY_ADDR | reg;
  LPC_EMAC->MWTD = value;
  mii_wait();
  return (LPC_EMAC->MIND & MIND_BUSY) == 0;
}

static int phy_read(uint32_t reg)
{
  LPC_EMAC->MADR = PHY_ADDR | reg;
  LPC_EMAC->MCMD = MCMD_READ;
  mii_wait();
  LPC_EMAC->MCMD = 0;
  if (LPC_EMAC->MIND & MIND_BUSY) {
    return -1;
  }
  return (int)(LPC_EMAC->MRDD & 0xFFFFu);
}

static void set_mac_mode(void)
{
  uint32_t cmd = CR_RX_EN | CR_TX_EN | CR_RMII | CR_PASS_RUNT;
  uint32_t mac2 = MAC2_CRC_EN | MAC2_PAD_EN;
  if (full_duplex) {
    cmd |= CR_FULL_DUP;
    mac2 |= MAC2_FULL_DUP;
  }
  LPC_EMAC->MAC2 = mac2;
  LPC_EMAC->IPGT = full_duplex ? IPGT_FULL_DUP : IPGT_HALF_DUP;
  LPC_EMAC->SUPP = speed_100 ? SUPP_SPEED : 0;
  LPC_EMAC->Command = cmd;
}

static void configure_pins(void)
{
  // RMII on P1.0, P1.1, P1.4, P1.8-P1.10, P1.14-P1.17.
  // Masked so the limit and spindle pins on P1.22+ stay GPIO.
  LPC_PINCON->PINSEL2 |= (1u << 0) | (1u << 2) | (1u << 8) | (1u << 16) |
                         (1u << 18) | (1u << 20) | (1u << 28) | (1u << 30);
  LPC_PINCON->PINSEL2 &= ~((1u << 1) | (1u << 3) | (1u << 9) | (1u << 17) |
                           (1u << 19) | (1u << 21) | (1u << 29) | (1u << 31));
  LPC_PINCON->PINSEL3 |= (1u << 0) | (1u << 2);
  LPC_PINCON->PINSEL3 &= ~((1u << 1) | (1u << 3));
}

bool emac_init(const uint8_t mac[6])
{
  configure_pins();
  LPC_SC->PCONP |= (1u << 30);

  LPC_EMAC->MAC1 = MAC1_RES_TX | MAC1_RES_MCS_TX | MAC1_RES_RX | MAC1_RES_MCS_RX |
                   MAC1_SIM_RES | MAC1_SOFT_RES;
  LPC_EMAC->Command = CR_REG_RES | CR_TX_RES | CR_RX_RES;
  for (volatile uint32_t d = 0; d < 256u; d++) { }

  LPC_EMAC->MAC1 = MAC1_PASS_ALL;
  LPC_EMAC->MAXF = MAX_FRAME_LEN;
  LPC_EMAC->CLRT = CLRT_DEF;
  LPC_EMAC->IPGR = IPGR_DEF;

  const uint8_t divs[] = {4, 6, 8, 10, 14, 20, 28, 36, 40, 44, 48, 52};
  uint32_t sel = 11;
  for (uint32_t i = 0; i < sizeof(divs); i++) {
    if ((uint32_t)divs[i] * 2500000u >= SystemCoreClock) {
      sel = i;
      break;
    }
  }
  LPC_EMAC->MCFG = MCFG_CLK_SEL(sel) | MCFG_RES_MII;
  for (volatile uint32_t d = 0; d < 256u; d++) { }
  LPC_EMAC->MCFG = MCFG_CLK_SEL(sel);

  LPC_EMAC->Command = CR_RMII;
  LPC_EMAC->SUPP = SUPP_RES_RMII;
  for (volatile uint32_t d = 0; d < 256u; d++) { }
  full_duplex = true;
  speed_100 = true;
  LPC_EMAC->SUPP = SUPP_SPEED;
  LPC_EMAC->MAC2 = MAC2_CRC_EN | MAC2_PAD_EN | MAC2_FULL_DUP;
  LPC_EMAC->IPGT = IPGT_FULL_DUP;

  if (!phy_write(PHY_BMCR, PHY_BMCR_RESET)) {
    return false;
  }
  bool reset_done = false;
  for (int i = 0; i < 100; i++) {
    for (volatile uint32_t d = 0; d < 10000u; d++) { }
    int bmcr = phy_read(PHY_BMCR);
    if (bmcr >= 0 && (bmcr & PHY_BMCR_RESET) == 0) {
      reset_done = true;
      break;
    }
  }
  if (!reset_done) {
    return false;
  }

  int id1 = phy_read(PHY_IDR1);
  int id2 = phy_read(PHY_IDR2);
  if (id1 < 0 || id2 < 0) {
    return false;
  }
  uint32_t phy_id = ((uint32_t)id1 << 16) | ((uint32_t)id2 & 0xFFF0u);
  if (phy_id != PHY_ID_8720A) {
    return false;
  }
  phy_write(PHY_BMCR, PHY_BMCR_AN);

  LPC_EMAC->SA0 = ((uint32_t)mac[5] << 8) | mac[4];
  LPC_EMAC->SA1 = ((uint32_t)mac[3] << 8) | mac[2];
  LPC_EMAC->SA2 = ((uint32_t)mac[1] << 8) | mac[0];

  for (int i = 0; i < N_RX; i++) {
    eth.rx_desc[i].packet = (uint32_t)eth.rx_buf[i];
    eth.rx_desc[i].control = (MAX_FRAME_LEN - 1u) | RCTRL_INT;
    eth.rx_stat[i].info = 0;
    eth.rx_stat[i].extra = 0;
  }
  for (int i = 0; i < N_TX; i++) {
    eth.tx_desc[i].packet = (uint32_t)eth.tx_buf[i];
    eth.tx_desc[i].control = (MAX_FRAME_LEN - 1u) | TCTRL_PAD | TCTRL_CRC | TCTRL_LAST | TCTRL_INT;
    eth.tx_stat[i].info = 0;
    eth.tx_stat[i].extra = 0;
  }

  LPC_EMAC->RxDescriptor = (uint32_t)eth.rx_desc;
  LPC_EMAC->RxStatus = (uint32_t)eth.rx_stat;
  LPC_EMAC->RxDescriptorNumber = N_RX - 1;
  LPC_EMAC->RxConsumeIndex = 0;

  LPC_EMAC->TxDescriptor = (uint32_t)eth.tx_desc;
  LPC_EMAC->TxStatus = (uint32_t)eth.tx_stat;
  LPC_EMAC->TxDescriptorNumber = N_TX - 1;
  LPC_EMAC->TxProduceIndex = 0;

  LPC_EMAC->RxFilterCtrl = RFC_UCAST_EN | RFC_BCAST_EN | RFC_PERFECT_EN;
  LPC_EMAC->IntEnable = 0;
  LPC_EMAC->IntClear = 0xFFFFu;

  set_mac_mode();
  applied_full = full_duplex;
  applied_100 = speed_100;
  LPC_EMAC->MAC1 |= MAC1_REC_EN;
  mac_ready = true;
  rx_index = 0;
  return true;
}

int emac_rx(uint8_t **frame)
{
  if (!mac_ready) {
    return 0;
  }
  while (LPC_EMAC->RxProduceIndex != LPC_EMAC->RxConsumeIndex) {
    rx_index = (int)LPC_EMAC->RxConsumeIndex;
    uint32_t info = eth.rx_stat[rx_index].info;
    int len = (int)((info & RINFO_SIZE) + 1u);
    if ((info & (RINFO_ERR | RINFO_LAST)) != RINFO_LAST || len < 14 || len > (int)MAX_FRAME_LEN) {
      emac_rx_done();
      continue;
    }
    *frame = eth.rx_buf[rx_index];
    return len;
  }
  return 0;
}

void emac_rx_done(void)
{
  uint32_t next = LPC_EMAC->RxConsumeIndex + 1u;
  if (next > LPC_EMAC->RxDescriptorNumber) {
    next = 0;
  }
  LPC_EMAC->RxConsumeIndex = next;
}

uint8_t *emac_tx_buffer(void)
{
  if (!mac_ready) {
    return 0;
  }
  uint32_t next = LPC_EMAC->TxProduceIndex + 1u;
  if (next > LPC_EMAC->TxDescriptorNumber) {
    next = 0;
  }
  if (next == LPC_EMAC->TxConsumeIndex) {
    return 0;
  }
  return eth.tx_buf[LPC_EMAC->TxProduceIndex];
}

bool emac_tx_commit(int len)
{
  if (!mac_ready || len < 14 || len > (int)MAX_FRAME_LEN) {
    return false;
  }
  uint32_t slot = LPC_EMAC->TxProduceIndex;
  uint32_t next = slot + 1u;
  if (next > LPC_EMAC->TxDescriptorNumber) {
    next = 0;
  }
  if (next == LPC_EMAC->TxConsumeIndex) {
    return false;
  }
  eth.tx_desc[slot].control = ((uint32_t)(len - 1) & 0x7FFu) | TCTRL_PAD | TCTRL_CRC | TCTRL_LAST | TCTRL_INT;
  __DSB();
  LPC_EMAC->TxProduceIndex = next;
  return true;
}

bool emac_link_up(void)
{
  if (!mac_ready) {
    return false;
  }
  // BMSR link status latches low. The second read is the current state.
  (void)phy_read(PHY_BMSR);
  int bmsr = phy_read(PHY_BMSR);
  if (bmsr < 0 || (bmsr & PHY_BMSR_LINK) == 0 || (bmsr & PHY_BMSR_AN_DONE) == 0) {
    return false;
  }
  int scsr = phy_read(PHY_SCSR);
  if (scsr >= 0) {
    switch ((scsr >> 2) & 7) {
      case 1: speed_100 = false; full_duplex = false; break;
      case 5: speed_100 = false; full_duplex = true; break;
      case 2: speed_100 = true; full_duplex = false; break;
      case 6: speed_100 = true; full_duplex = true; break;
      default: break;
    }
  }
  if (full_duplex != applied_full || speed_100 != applied_100) {
    set_mac_mode();
    applied_full = full_duplex;
    applied_100 = speed_100;
  }
  return true;
}
