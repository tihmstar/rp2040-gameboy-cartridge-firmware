#include "GbRtc.h"
#include "GlobalDefines.h"
#include <stdint.h>

#include <hardware/timer.h>

static volatile uint8_t _registerMasks[] = {0x3f, 0x3f, 0x1f, 0xff, 0xc1};
static uint8_t _currentRegister = 0;

static uint64_t _lastMilli = 0;
static uint32_t _millies = 0;

extern volatile uint64_t g_lastFakeTimestampUpdate;
extern volatile uint64_t g_fakeTimestamp;

void __no_inline_not_in_flash_func(GbRtc_WriteRegister)(uint8_t val) {
  const uint8_t oldHalt = g_rtcReal.reg.status.halt;

  g_rtcReal.asArray[_currentRegister] = val & _registerMasks[_currentRegister];

  if (_currentRegister == 0) {
    _lastMilli = time_us_64();
    _millies = 0;
  }

  if (oldHalt && !g_rtcReal.reg.status.halt) {
    _lastMilli = time_us_64();
  }
}

void __no_inline_not_in_flash_func(GbRtc_ActivateRegister)(uint8_t reg) {
  if (reg >= sizeof(_registerMasks)) {
    return;
  }

  _rtcLatchPtr = &g_rtcLatched.asArray[reg];
  _currentRegister = reg;
}

void __no_inline_not_in_flash_func(GbRtc_PerformRtcTick)() {
  uint64_t now = time_us_64();
  uint8_t registerToMask = 0;

  {
    if ((uint32_t)(now - g_lastFakeTimestampUpdate) >= USEC_PER_SEC){
      g_fakeTimestamp++;
      g_lastFakeTimestampUpdate += USEC_PER_SEC;
    }
  }

  if (!g_rtcReal.reg.status.halt) {
    if ((now - _lastMilli) > 1000U) {
      _lastMilli += 1000;
      _millies++;
    }

    if (_millies >= 1000) {
      g_rtcReal.reg.seconds++;
      _millies = 0;
      g_rtcReal.asArray[registerToMask] &= _registerMasks[registerToMask];
      registerToMask++;

      if (g_rtcReal.reg.seconds == 60) {
        g_rtcReal.reg.seconds = 0;
        g_rtcReal.reg.minutes++;

        g_rtcReal.asArray[registerToMask] &= _registerMasks[registerToMask];
        registerToMask++;

        if (g_rtcReal.reg.minutes == 60) {
          g_rtcReal.reg.minutes = 0;
          g_rtcReal.reg.hours++;

          g_rtcReal.asArray[registerToMask] &= _registerMasks[registerToMask];
          registerToMask++;
        }

        if (g_rtcReal.reg.hours == 24) {
          g_rtcReal.reg.hours = 0;
          g_rtcReal.reg.days++;

          g_rtcReal.asArray[registerToMask] &= _registerMasks[registerToMask];
          registerToMask++;

          if (g_rtcReal.reg.days == 0) {
            if (g_rtcReal.reg.status.days_high) {
              g_rtcReal.reg.status.days_carry = 1;
            }
            g_rtcReal.reg.status.days_high++;
          }
        }
      }
    }
  }
}

void GbRtc_ProgressRtcWithSeconds(union GbRtcUnion *gbrtc, uint64_t elapsedSeconds){
  uint8_t seconds = elapsedSeconds % 60;
  gbrtc->reg.seconds += seconds;
  while (gbrtc->reg.seconds >= 60){
    gbrtc->reg.seconds -= 60;
    gbrtc->reg.minutes++;
  }
  elapsedSeconds /= 60;

  uint8_t minutes = elapsedSeconds % 60;
  gbrtc->reg.minutes += minutes;
  while (gbrtc->reg.minutes >= 60){
    gbrtc->reg.minutes -= 60;
    gbrtc->reg.hours++;
  }
  elapsedSeconds /= 60;

  uint8_t hours = elapsedSeconds % 24;
  gbrtc->reg.hours += hours;
  while (gbrtc->reg.hours >= 24){
    gbrtc->reg.hours -= 24;
    if (++gbrtc->reg.days == 0) gbrtc->reg.status.days_high = 1;
  }
  elapsedSeconds /= 24;

  if (elapsedSeconds + gbrtc->reg.days > 0xff) gbrtc->reg.status.days_high = 1;
  gbrtc->reg.days = (uint8_t)(elapsedSeconds + gbrtc->reg.days);
}
