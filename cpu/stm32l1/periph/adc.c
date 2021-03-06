/*
 * Copyright (C) 2016 Fundacion Inria Chile
 *
 * This file is subject to the terms and conditions of the GNU Lesser General
 * Public License v2.1. See the file LICENSE in the top level directory for more
 * details.
 */

/**
 * @ingroup     cpu_stm32l1
 * @ingroup     drivers_periph_adc
 * @{
 *
 * @file
 * @brief       Low-level ADC driver implementation
 *
 * @author      Francisco Molina <francisco.molina@inria.cl>
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 * @author      Nick v. IJzendoorn <nijzendoorn@engineering-spirit.nl>
 *
 * @}
 */

#include "cpu.h"
#include "mutex.h"
#include "periph/adc.h"

/**
 * @brief   ADC clock settings
 *
 * NB: with ADC_CLOCK_HIGH, Vdda should be 2.4V min
 *
 */
#define ADC_CLOCK_HIGH      (0)
#define ADC_CLOCK_MEDIUM    (ADC_CCR_ADCPRE_0)
#define ADC_CLOCK_LOW       (ADC_CCR_ADCPRE_1)

/**
 * @brief   ADC sample time, cycles
 */
#define ADC_SAMPLE_TIME_4C    (0)
#define ADC_SAMPLE_TIME_9C    (1)
#define ADC_SAMPLE_TIME_16C   (2)
#define ADC_SAMPLE_TIME_24C   (3)
#define ADC_SAMPLE_TIME_48C   (4)
#define ADC_SAMPLE_TIME_96C   (5)
#define ADC_SAMPLE_TIME_192C  (6)
#define ADC_SAMPLE_TIME_384C  (7)

/**
 * @brief   Load the ADC configuration
 * @{
 */
#ifdef ADC_CONFIG
static const adc_conf_t adc_config[] = ADC_CONFIG;
#else
static const adc_conf_t adc_config[] = {};
#endif

/**
 * @brief   Allocate locks for all three available ADC device
 *
 * All STM32l1 CPU's have single ADC device
 */
static mutex_t lock = MUTEX_INIT;

static inline void prep(void)
{
    mutex_lock(&lock);
    /* ADC clock is always HSI clock */
    if (!(RCC->CR & RCC_CR_HSION)) {
        RCC->CR |= RCC_CR_HSION;
        /* Wait for HSI to become ready */
        while (!(RCC->CR & RCC_CR_HSION)) {}
    }

    periph_clk_en(APB2, RCC_APB2ENR_ADC1EN);
}

static inline void done(void)
{
    periph_clk_dis(APB2, RCC_APB2ENR_ADC1EN);

    mutex_unlock(&lock);
}

static void adc_set_sample_time(uint8_t time)
{
    uint8_t i;
    uint32_t reg32 = 0;

    for (i = 0; i <= 9; i++) {
        reg32 |= (time << (i * 3));
    }
#if !defined STM32L1XX_MD
    ADC1->SMPR0 = reg32;
#endif
    ADC1->SMPR1 = reg32;
    ADC1->SMPR2 = reg32;
    ADC1->SMPR3 = reg32;
}

int adc_init(adc_t line)
{
    /* check if the line is valid */
    if (line >= ADC_NUMOF) {
        return -1;
    }

    /* lock and power-on the device */
    prep();

    /* configure the pin */
    if ((adc_config[line].pin != GPIO_UNDEF))
        gpio_init_analog(adc_config[line].pin);

    /* set ADC clock prescaler */
    ADC->CCR &= ~ADC_CCR_ADCPRE;
    ADC->CCR |= ADC_CLOCK_MEDIUM;

    /* Set sample time */
    /* Min 4us needed for temperature sensor measurements */
    switch (ADC->CCR & ADC_CCR_ADCPRE) {
        case ADC_CLOCK_LOW:
            /* 4 MHz ADC clock -> 16 cycles */
            adc_set_sample_time(ADC_SAMPLE_TIME_16C);
            break;
        case ADC_CLOCK_MEDIUM:
            /* 8 MHz ADC clock -> 48 cycles */
            adc_set_sample_time(ADC_SAMPLE_TIME_48C);
            break;
        default:
            /* 16 MHz ADC clock -> 96 cycles */
            adc_set_sample_time(ADC_SAMPLE_TIME_96C);
    }

    /* enable the ADC module */
    ADC1->CR2 = ADC_CR2_ADON;
    /* turn off during idle phase*/
    ADC1->CR1 = ADC_CR1_PDI;
    
    /* check if this channel is an internal ADC channel, if so
     * enable the internal temperature and Vref */
    if (adc_config[line].chan == ADC_TEMPERATURE_CHANNEL || adc_config[line].chan == ADC_VREF_CHANNEL) {
        ADC->CCR |= ADC_CCR_TSVREFE;
        while ((PWR->CSR & PWR_CSR_VREFINTRDYF) == 0);
    }
    
    /* Wait for ADC to become ready */
	while ((ADC1->SR & ADC_SR_ADONS) == 0);

    /* free the device again */
    done();

    return 0;
}

#define CR1_CLEAR_MASK            ((uint32_t)0xFCFFFEFF)
#define CR2_CLEAR_MASK            ((uint32_t)0xC0FFF7FD)

int adc_sample(adc_t line,  adc_res_t res)
{
    int sample;

    /* check if resolution is applicable */
    if ( (res != ADC_RES_6BIT) &&
         (res != ADC_RES_8BIT) &&
         (res != ADC_RES_10BIT) &&
         (res != ADC_RES_12BIT)) {
        return -1;
    }

    /* lock and power on the ADC device  */
    prep();
    
    /* set resolution, conversion channel and single read */
    ADC1->CR1 |= res & ADC_CR1_RES;
    ADC1->SQR1 &= ~ADC_SQR1_L;
    ADC1->SQR5 = adc_config[line].chan;

    /* wait for regular channel to be ready*/
    while (!(ADC1->SR & ADC_SR_RCNR)) {}
    /* start conversion and wait for results */
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC)) {}
    /* finally read sample and reset the STRT bit in the status register */
    sample = (int)ADC1->DR;
    ADC1 -> SR &= ~ADC_SR_STRT;
    
    int cal_vref, cal_ts1, cal_ts2;
    /* In case of VREF channel calculate and return actual VDD, not Vref */
	if (adc_config[line].chan == ADC_VREF_CHANNEL) {
        if (cpu_status.category < 3) {
            /* low-end devices doesn't provide calibration values, see errata */
            cal_vref = 1672;
        } else {
            cal_vref = *(uint16_t *)ADC_VREFINT_CAL;
        }
        /* calibration value is for ADC_RES_12BIT, adjust for it if needed */
        switch (res) {
            case ADC_RES_6BIT:
                sample = sample << 6;
                break;
            case ADC_RES_8BIT:
                sample = sample << 4;
                break;
            case ADC_RES_10BIT:
                sample = sample << 2;
                break;
            default:
                break;
        }
        
        sample = (3000 * cal_vref) / sample;
	}
    
    /* in case of temperature channel sample VDD too */
    int sample_vref = 0;
    if (adc_config[line].chan == ADC_TEMPERATURE_CHANNEL) {
        ADC1->SQR5 = ADC_VREF_CHANNEL;
        /* wait for regulat channel to be ready*/
        while (!(ADC1->SR & ADC_SR_RCNR)) {}
        
        /* start conversion and wait for results */
        ADC1->CR2 |= ADC_CR2_SWSTART;
        while (!(ADC1->SR & ADC_SR_EOC)) {}
        
        sample_vref = (int)ADC1->DR;
        ADC1 -> SR &= ~ADC_SR_STRT;
        
        /* calibrate temperature data */
        if (cpu_status.category < 3) {
            /* low-end devices doesn't provide calibration values, see errata */
            /* values according to STM32L151x6/8/B-A datasheet, tables 17 and 59 */
            cal_ts1   = 680;
            cal_ts2   = 856;
            cal_vref  = 1671;
        } else {
            cal_ts1   = *(uint16_t *)ADC_TS_CAL1;
            cal_ts2   = *(uint16_t *)ADC_TS_CAL2;
            cal_vref  = *(uint16_t *)ADC_VREFINT_CAL;
        }
        /* calibration values are for ADC_RES_12BIT, adjust for it if needed */
        switch (res) {
            case ADC_RES_6BIT:
                sample = sample << 6;
                sample_vref = sample_vref << 6;
                break;
            case ADC_RES_8BIT:
                sample = sample << 4;
                sample_vref = sample_vref << 4;
                break;
            case ADC_RES_10BIT:
                sample = sample << 2;
                sample_vref = sample_vref << 2;
                break;
            default:
                break;
        }
        
        /* Adjust temperature sensor data for actual VDD */
        sample = (cal_vref * sample)/sample_vref;
        
        /* return chip temperature, 1 C resolution */
        sample = 30 + (80*(sample - cal_ts1))/(cal_ts2 - cal_ts1);
    }
       
    /* Disable temperature and Vref conversion */
	ADC->CCR &= ~ADC_CCR_TSVREFE;
    
    /* power off and unlock device again */
    done();

    return sample;
}