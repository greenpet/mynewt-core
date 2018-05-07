/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include "pwm_stm32/pwm_stm32.h"

#include <bsp.h>
#include <hal/hal_bsp.h>
#include <mcu/cmsis_nvic.h>
#include <os/os.h>
#include <pwm/pwm.h>
#include <stm32/stm32_hal.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

#if MYNEWT_VAL(MCU_STM32F3)
#   include "mcu/stm32f3_bsp.h"
#   include "stm32f3xx.h"
#   include "stm32f3xx_hal.h"
#   include "stm32f3xx_hal_tim.h"
#elif MYNEWT_VAL(MCU_STM32F7)
#   include "mcu/stm32f7_bsp.h"
#   include "stm32f7xx.h"
#   include "stm32f7xx_ll_bus.h"
#   include "stm32f7xx_ll_tim.h"
#else
#   error "MCU not yet supported."
#endif

#define STM32_PWM_CH_MAX      4
#define STM32_PWM_CH_DISABLED 0x0FFFFFFF
#define STM32_PWM_CH_NOPIN    0xFF
#define STM32_PWM_CH_NOAF     0x0F

typedef void (*stm32_pwm_isr_t)(void);

typedef struct {
    union {
        uint32_t      config;
        struct {
            uint32_t  duty:16;
            uint32_t  pin:8;
            uint32_t  af:4;
            uint32_t  invert:1;     /* invert output */
            uint32_t  enabled:1;    /* channel enabled */
            uint32_t  update:1;     /* channel needs update */
            uint32_t  configure:1;  /* channel needs HW configuration step */
        };
    };
    uint32_t cycle_count;
    uint32_t cycle;
    user_handler_t cycle_callback;
    user_handler_t sequence_callback;
    void *cycle_data;
    void *sequence_data;
} stm32_pwm_ch_t;

typedef struct {
    TIM_TypeDef       *timx;
    stm32_pwm_ch_t    ch[STM32_PWM_CH_MAX];
} stm32_pwm_dev_t;

static stm32_pwm_dev_t stm32_pwm_dev[PWM_COUNT];

static uint32_t
stm32_pwm_ch(int ch)
{
    switch (ch) {
        case 0: return LL_TIM_CHANNEL_CH1;
        case 1: return LL_TIM_CHANNEL_CH2;
        case 2: return LL_TIM_CHANNEL_CH3;
        case 3: return LL_TIM_CHANNEL_CH4;
    }
    assert(0);
    return 0;
}

static void
stm32_pwm_ch_set_compare(TIM_TypeDef *tim, int ch, uint32_t value)
{
    switch (ch) {
        case 0:
            LL_TIM_OC_SetCompareCH1(tim, value);
            break;
        case 1:
            LL_TIM_OC_SetCompareCH2(tim, value);
            break;
        case 2:
            LL_TIM_OC_SetCompareCH3(tim, value);
            break;
        case 3:
            LL_TIM_OC_SetCompareCH4(tim, value);
            break;
    }
}

static inline bool
stm32_pwm_has_assigned_pin(const stm32_pwm_ch_t *ch)
{
    return STM32_PWM_CH_NOPIN != ch->pin && STM32_PWM_CH_NOAF != ch->af;
}

static int
stm32_pwm_disable_ch(stm32_pwm_dev_t *pwm, uint8_t cnum)
{
    LL_TIM_CC_DisableChannel(pwm->timx, stm32_pwm_ch(cnum));

    if (stm32_pwm_has_assigned_pin(&pwm->ch[cnum])) {
        /* unconfigure the previously used pin */
        if (hal_gpio_init_af(pwm->ch[cnum].pin,
                             0,
                             HAL_GPIO_PULL_NONE,
                             0)) {
            return STM32_PWM_ERR_GPIO;
        }
    }

    pwm->ch[cnum].config = STM32_PWM_CH_DISABLED;

    return STM32_PWM_ERR_OK;
}

/*
 * This could be more efficient by using different implementations of ISRS for the
 * individual timers. But some timers share the interrupt anyway so we would still
 * have to go and look for the trigger. And, the number of PWM peripherals is most
 * likely rather low.
 */
static void
stm32_pwm_isr()
{
    for (int i=0; i<PWM_COUNT; ++i) {
        stm32_pwm_dev_t *pwm = &stm32_pwm_dev[i];
        uint32_t sr = pwm->timx->SR;
        pwm->timx->SR = ~sr;

        if (sr & TIM_SR_UIF) {
            for (int j=0; j<STM32_PWM_CH_MAX; ++j) {
                stm32_pwm_ch_t *ch = &pwm->ch[j];
                if (ch->enabled) {
                    if (1 == ch->cycle) {
                        ch->cycle = ch->cycle_count;
                        if (ch->sequence_callback) {
                            ch->sequence_callback(ch->sequence_data);
                        } else {
                            stm32_pwm_disable_ch(pwm, j);
                        }
                    } else {
                        if (ch->cycle) {
                            --ch->cycle;
                        }
                        if (ch->cycle_callback) {
                            ch->cycle_callback(ch->cycle_data);
                        }
                    }
                }
            }
        }
    }
}


static int
stm32_pwm_open(struct os_dev *odev, uint32_t wait, void *arg)
{
    struct pwm_dev *dev;
    int rc;

    dev = (struct pwm_dev *)odev;
    assert(dev);

    if (os_started()) {
        rc = os_mutex_pend(&dev->pwm_lock, wait);
        if (OS_OK != rc) {
            return rc;
        }
    }

    if (odev->od_flags & OS_DEV_F_STATUS_OPEN) {
        os_mutex_release(&dev->pwm_lock);
        rc = OS_EBUSY;
        return rc;
    }

    return STM32_PWM_ERR_OK;
}

static int
stm32_pwm_close(struct os_dev *odev)
{
    struct pwm_dev *dev;

    dev = (struct pwm_dev *)odev;
    assert(dev);

    if (os_started()) {
        os_mutex_release(&dev->pwm_lock);
    }

    return STM32_PWM_ERR_OK;
}

static int
stm32_pwm_update_channels(stm32_pwm_dev_t *pwm, bool update_all)
{
    int configured = 0;
    int enabled = 0;

    for (int i=0; i<STM32_PWM_CH_MAX; ++i) {
        if (STM32_PWM_CH_DISABLED != pwm->ch[i].config) {
            ++enabled;
            if (pwm->ch[i].enabled && (update_all || pwm->ch[i].update)) {
                if (pwm->ch[i].configure) {
                    ++configured;
                    uint32_t channelID = stm32_pwm_ch(i);
                    LL_TIM_OC_SetMode(pwm->timx, channelID, LL_TIM_OCMODE_PWM1);
                    LL_TIM_OC_SetPolarity(pwm->timx, channelID, pwm->ch[i].invert ? LL_TIM_OCPOLARITY_HIGH : LL_TIM_OCPOLARITY_LOW);
                    LL_TIM_OC_EnablePreload(pwm->timx,  channelID);

                    stm32_pwm_ch_set_compare(pwm->timx, i, pwm->ch[i].duty);

                    LL_TIM_CC_EnableChannel(pwm->timx, channelID);

                    pwm->ch[i].configure = false;
                } else {
                    stm32_pwm_ch_set_compare(pwm->timx, i, pwm->ch[i].duty);
                }

                pwm->ch[i].update = false;
            }
        }
    }

    if (0 == enabled) {
        LL_TIM_DisableCounter(pwm->timx);
    } else {
        if (enabled == configured) {
            LL_TIM_SetCounter(pwm->timx, 0);
            LL_TIM_GenerateEvent_UPDATE(pwm->timx);
            LL_TIM_EnableCounter(pwm->timx);
        }
    }
    return STM32_PWM_ERR_OK;
}


static int
stm32_pwm_configure_channel(struct pwm_dev *dev, uint8_t cnum, struct pwm_chan_cfg *cfg)
{
    stm32_pwm_dev_t *pwm;
    stm32_pwm_ch_t *ch;
    uint8_t af;

    assert(dev);
    assert(dev->pwm_instance_id < PWM_COUNT);
    if (cnum >= dev->pwm_chan_count) {
        return STM32_PWM_ERR_CHAN;
    }

    pwm = &stm32_pwm_dev[dev->pwm_instance_id];
    ch = &pwm->ch[cnum];
    af = (uint8_t)(uintptr_t)cfg->data & 0x0F;

    if (cfg->pin != ch->pin || af != ch->af) {
        if (stm32_pwm_has_assigned_pin(ch)) {
            /* unconfigure the previously used pin */
            if (hal_gpio_init_af(ch->pin, 0, HAL_GPIO_PULL_NONE, 0)) {
                return STM32_PWM_ERR_GPIO;
            }
        }

        if (STM32_PWM_CH_NOPIN != cfg->pin && STM32_PWM_CH_NOAF != af) {
            /* configure the assigned pin */
            if (hal_gpio_init_af(cfg->pin, af, HAL_GPIO_PULL_NONE, 0)) {
                return STM32_PWM_ERR_GPIO;
            }
        }
    }

    ch->pin         = cfg->pin;
    ch->af          = af;
    ch->invert      = cfg->inverted;
    ch->update      = ch->enabled;
    ch->configure   = true;

    ch->cycle_count = cfg->n_cycles;
    ch->cycle       = cfg->n_cycles;

    if (cfg->interrupts_cfg) {
        struct pwm_dev_interrupt_cfg *icfg = (struct pwm_dev_interrupt_cfg*)cfg;
        ch->cycle_callback    = icfg->cycle_handler;
        ch->cycle_data        = icfg->cycle_data;
        ch->sequence_callback = icfg->seq_end_handler;
        ch->sequence_data     = icfg->seq_end_data;
    } else {
        ch->cycle_callback    = 0;
        ch->cycle_data        = 0;
        ch->sequence_callback = 0;
        ch->sequence_data     = 0;
    }

    return stm32_pwm_update_channels(pwm, false);
}


static int
stm32_pwm_enable_duty_cycle(struct pwm_dev *dev, uint8_t cnum, uint16_t fraction)
{
    stm32_pwm_dev_t *pwm;

    assert(dev);
    assert(dev->pwm_instance_id < PWM_COUNT);
    if (cnum >= dev->pwm_chan_count) {
        return STM32_PWM_ERR_CHAN;
    }

    pwm = &stm32_pwm_dev[dev->pwm_instance_id];
    pwm->ch[cnum].duty    = fraction;
    pwm->ch[cnum].update  = true;
    pwm->ch[cnum].enabled = true;

    return stm32_pwm_update_channels(pwm, false);
}

static int
stm32_pwm_disable(struct pwm_dev *dev, uint8_t cnum)
{
    stm32_pwm_dev_t *pwm;

    assert(dev);
    assert(dev->pwm_instance_id < PWM_COUNT);
    if (cnum >= dev->pwm_chan_count) {
        return STM32_PWM_ERR_CHAN;
    }

    pwm = &stm32_pwm_dev[dev->pwm_instance_id];

    return stm32_pwm_disable_ch(pwm, cnum);
}

static int
stm32_pwm_set_frequency(struct pwm_dev *dev, uint32_t freq_hz)
{
    stm32_pwm_dev_t *pwm;
    uint32_t id;
    uint32_t timer_clock;
    uint32_t div, div1, div2;

    assert(dev);
    assert(dev->pwm_instance_id < PWM_COUNT);

    if (!freq_hz) {
        return STM32_PWM_ERR_FREQ;
    }

    id = dev->pwm_instance_id;
    pwm = &stm32_pwm_dev[id];

    timer_clock = stm32_hal_timer_get_freq(pwm->timx);
    assert(timer_clock);

    div  = timer_clock / freq_hz;
    if (!div) {
        return STM32_PWM_ERR_FREQ;
    }

    div1 = div >> 16;
    div2 = div / (div1 + 1);

    if (div1 > div2) {
        uint32_t tmp = div1;
        div1 = div2;
        div2 = tmp;
    }
    div2 -= 1;

    LL_TIM_SetPrescaler(pwm->timx, div1);
    LL_TIM_SetAutoReload(pwm->timx, div2);

    return stm32_pwm_update_channels(pwm, true);
}

static int
stm32_pwm_get_clock_freq(struct pwm_dev *dev)
{
    stm32_pwm_dev_t *pwm;

    assert(dev);
    assert(dev->pwm_instance_id < PWM_COUNT);

    pwm = &stm32_pwm_dev[dev->pwm_instance_id];
    return stm32_hal_timer_get_freq(pwm->timx) / (LL_TIM_GetPrescaler(pwm->timx) + 1);
}

static int
stm32_pwm_get_top_value(struct pwm_dev *dev)
{
    stm32_pwm_dev_t *pwm;

    assert(dev);
    assert(dev->pwm_instance_id < PWM_COUNT);

    pwm = &stm32_pwm_dev[dev->pwm_instance_id];
    return LL_TIM_GetAutoReload(pwm->timx) + 1;
}

static int
stm32_pwm_get_resolution_bits(struct pwm_dev *dev)
{
    uint32_t period = stm32_pwm_get_top_value(dev) - 1;

    for (int bit=15; bit; --bit) {
        if (period & (0x01 << bit)) {
            return bit + 1;
        }
    }

    return period ? 1 : 0;
}


int
stm32_pwm_dev_init(struct os_dev *odev, void *arg)
{
    stm32_pwm_conf_t *cfg;
    stm32_pwm_dev_t *pwm;
    struct pwm_dev *dev;
    uint32_t id;

    for (id = 0; id < PWM_COUNT; ++id) {
        pwm = &stm32_pwm_dev[id];
        if (!pwm->timx) {
            break;
        }
    }
    if (PWM_COUNT <= id) {
        return STM32_PWM_ERR_NODEV;
    }

    if (NULL == arg) {
        return STM32_PWM_ERR_NOTIM;
    }

    cfg = (stm32_pwm_conf_t*)arg;

    pwm->timx = cfg->tim;

    LL_TIM_SetPrescaler(cfg->tim, 0xffff);
    LL_TIM_SetAutoReload(cfg->tim, 0);

    dev = (struct pwm_dev *)odev;
    dev->pwm_instance_id = id;

    switch ((uintptr_t)cfg->tim) {
#ifdef TIM1
      case (uintptr_t)TIM1:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM1);
          dev->pwm_chan_count = 4;
          break;
#endif
#ifdef TIM2
      case (uintptr_t)TIM2:
          LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM2);
          dev->pwm_chan_count = 4;
          break;
#endif
#ifdef TIM3
      case (uintptr_t)TIM3:
          LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);
          dev->pwm_chan_count = 4;
          break;
#endif
#ifdef TIM4
      case (uintptr_t)TIM4:
          LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM4);
          dev->pwm_chan_count = 4;
          break;
#endif
#ifdef TIM5
      case (uintptr_t)TIM5:
          LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM5);
          dev->pwm_chan_count = 4;
          break;
#endif

      /* basic timers TIM6 and TIM7 have no PWM capabilities */

#ifdef TIM8
      case (uintptr_t)TIM8:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM8);
          dev->pwm_chan_count = 4;
          break;
#endif
#ifdef TIM9
      case (uintptr_t)TIM9:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM9);
          dev->pwm_chan_count = 2;
          break;
#endif
#ifdef TIM10
      case (uintptr_t)TIM10:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM10);
          dev->pwm_chan_count = 1;
          break;
#endif
#ifdef TIM11
      case (uintptr_t)TIM11:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM11);
          dev->pwm_chan_count = 1;
          break;
#endif
#ifdef TIM12
      case (uintptr_t)TIM12:
          LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM12);
          dev->pwm_chan_count = 2;
          break;
#endif
#ifdef TIM13
      case (uintptr_t)TIM13:
          LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM13);
          dev->pwm_chan_count = 1;
          break;
#endif
#ifdef TIM14
      case (uintptr_t)TIM14:
          LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM14);
          dev->pwm_chan_count = 1;
          break;
#endif
#ifdef TIM15
      case (uintptr_t)TIM15:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM15);
          dev->pwm_chan_count = 2;
          break;
#endif
#ifdef TIM16
      case (uintptr_t)TIM16:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM16);
          dev->pwm_chan_count = 1;
          break;
#endif
#ifdef TIM17
      case (uintptr_t)TIM17:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM17);
          dev->pwm_chan_count = 1;
          break;
#endif

      /* basic timer TIM18 has no PWM capabilities */

#ifdef TIM19
      case (uintptr_t)TIM19:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM19);
          dev->pwm_chan_count = 4;
          break;
#endif
#ifdef TIM20
      case (uintptr_t)TIM20:
          LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM20);
          dev->pwm_chan_count = 4;
          break;
#endif

      default:
          assert(0);
    }

    for (int i=0; i<STM32_PWM_CH_MAX; ++i) {
        pwm->ch[i].config = STM32_PWM_CH_DISABLED;
    }

    dev->pwm_funcs.pwm_configure_channel = stm32_pwm_configure_channel;
    dev->pwm_funcs.pwm_enable_duty_cycle = stm32_pwm_enable_duty_cycle;
    dev->pwm_funcs.pwm_set_frequency = stm32_pwm_set_frequency;
    dev->pwm_funcs.pwm_get_clock_freq = stm32_pwm_get_clock_freq;
    dev->pwm_funcs.pwm_get_resolution_bits = stm32_pwm_get_resolution_bits;
    dev->pwm_funcs.pwm_get_top_value = stm32_pwm_get_top_value;
    dev->pwm_funcs.pwm_disable = stm32_pwm_disable;

    os_mutex_init(&dev->pwm_lock);
    OS_DEV_SETHANDLERS(odev, stm32_pwm_open, stm32_pwm_close);

    LL_TIM_EnableARRPreload(cfg->tim);
    LL_TIM_EnableIT_UPDATE(cfg->tim);
    LL_TIM_CC_EnablePreload(cfg->tim);

    NVIC_SetPriority(cfg->irq, (1 << __NVIC_PRIO_BITS) - 1);
    NVIC_SetVector(cfg->irq, (uintptr_t)stm32_pwm_isr);
    NVIC_EnableIRQ(cfg->irq);

    return STM32_PWM_ERR_OK;
}
