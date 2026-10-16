#include "grbl.h"
#include "joystick.h"

// Counts either side of the measured center that still count as released.
#define JOYSTICK_DEADZONE 300
// Length of one queued jog piece. A short piece keeps a direction change quick.
#define JOYSTICK_SEGMENT_MS 40
// Set to 1 to swap an axis that moves the wrong way.
#define JOYSTICK_INVERT_X 0
#define JOYSTICK_INVERT_Y 0

static uint16_t center_x;
static uint16_t center_y;
static uint8_t busy;

static uint16_t adc_sample(volatile uint32_t *reg)
{
  return (uint16_t)((*reg >> 4) & 0x0FFF);
}

void joystick_init()
{
  uint8_t i;
  uint32_t sum_x = 0;
  uint32_t sum_y = 0;

  LPC_SC->PCONP |= (1u << 12); // PCADC
  LPC_PINCON->PINSEL1 = (LPC_PINCON->PINSEL1 & ~((3u << 16) | (3u << 18))) | (1u << 16) | (1u << 18);
  LPC_PINCON->PINMODE1 = (LPC_PINCON->PINMODE1 & ~((3u << 16) | (3u << 18))) | (2u << 16) | (2u << 18);
  // Channels AD0.1 and AD0.2, PCLK/2, burst, power on. 10-bit clocks are enough for a stick.
  LPC_ADC->ADCR = (1u << 1) | (1u << 2) | (1u << 8) | (1u << 16) | (1u << 21);

  delay_us(20);
  for (i = 0; i < 32; i++) {
    delay_us(20);
    sum_x += adc_sample(&LPC_ADC->ADDR1);
    sum_y += adc_sample(&LPC_ADC->ADDR2);
  }
  center_x = (uint16_t)(sum_x / 32);
  center_y = (uint16_t)(sum_y / 32);
}

static float axis_deflection(uint16_t sample, uint16_t center, uint8_t invert)
{
  int32_t delta = (int32_t)sample - (int32_t)center;
  if (delta > -JOYSTICK_DEADZONE && delta < JOYSTICK_DEADZONE) { return 0.0f; }
  if (delta > 0) { delta -= JOYSTICK_DEADZONE; }
  else { delta += JOYSTICK_DEADZONE; }
  float unit = (float)delta / (float)(2048 - JOYSTICK_DEADZONE);
  if (unit > 1.0f) { unit = 1.0f; }
  if (unit < -1.0f) { unit = -1.0f; }
  if (invert) { unit = -unit; }
  return unit;
}

void joystick_poll()
{
  float dx;
  float dy;
  float mag;
  float feed;
  float ref;
  float distance;
  uint8_t i;
  plan_line_data_t pl_data;
  parser_block_t block;

  if (busy) { return; }
  busy = 1;

  if (sys.suspend & SUSPEND_JOG_CANCEL) { busy = 0; return; }
  if (!(sys.state == STATE_IDLE || sys.state == STATE_JOG)) { busy = 0; return; }

  dx = axis_deflection(adc_sample(&LPC_ADC->ADDR1), center_x, JOYSTICK_INVERT_X);
  dy = axis_deflection(adc_sample(&LPC_ADC->ADDR2), center_y, JOYSTICK_INVERT_Y);
  mag = hypot_f(dx, dy);

  if (mag < 0.001f) {
    if (sys.state == STATE_JOG) { system_set_exec_state_flag(EXEC_MOTION_CANCEL); }
    busy = 0;
    return;
  }

  if (plan_get_block_buffer_count() >= 2) { busy = 0; return; }

  dx /= mag;
  dy /= mag;
  if (mag > 1.0f) { mag = 1.0f; }

  ref = settings.max_rate[X_AXIS];
  if (settings.max_rate[Y_AXIS] < ref) { ref = settings.max_rate[Y_AXIS]; }
  feed = mag * ref;
  distance = feed * ((float)JOYSTICK_SEGMENT_MS / 60000.0f);
  if (distance < 0.02f) { busy = 0; return; }

  if (sys.state == STATE_IDLE) { gc_sync_position(); }

  memset(&pl_data, 0, sizeof(pl_data));
  memset(&block, 0, sizeof(block));
  pl_data.condition = PL_COND_FLAG_NO_FEED_OVERRIDE;
  pl_data.spindle_speed = 0.0f;
  block.values.f = feed;
  for (i = 0; i < N_AXIS; i++) { block.values.xyza[i] = gc_state.position[i]; }
  block.values.xyza[X_AXIS] += dx * distance;
  block.values.xyza[Y_AXIS] += dy * distance;

  if (jog_execute(&pl_data, &block) == STATUS_OK) {
    memcpy(gc_state.position, block.values.xyza, sizeof(block.values.xyza));
  }
  busy = 0;
}
