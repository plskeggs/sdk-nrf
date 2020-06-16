/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>
#include <stdio.h>
#include <string.h>
//#include <drivers/gps.h>
//#include <drivers/sensor.h>
#include <console/console.h>
#include <nrf_cloud.h>
#include <dk_buttons_and_leds.h>
#include <lte_lc.h>
#include <power/reboot.h>
#include <net/bsdlib.h>

#include "ble.h"

/* Interval in milliseconds between each time status LEDs are updated. */
#define LEDS_UPDATE_INTERVAL          500

/* Interval in microseconds between each time LEDs are updated when indicating
 * that an error has occurred.
 */
#define LEDS_ERROR_UPDATE_INTERVAL      250000

#define LED_ON(x)     (x)
#define LED_BLINK(x)      ((x) << 8)
#define LED_GET_ON(x)     ((x) & 0xFF)
#define LED_GET_BLINK(x)    (((x) >> 8) & 0xFF)

#define NRF_CLOUD_EVT_BLE_CONNECT 0xAA


enum {
        LEDS_INITIALIZING     = LED_ON(0),
        LEDS_CONNECTING       = LED_BLINK(DK_LED3_MSK),
        LEDS_PATTERN_WAIT     = LED_BLINK(DK_LED3_MSK | DK_LED4_MSK),
        LEDS_PATTERN_ENTRY    = LED_ON(DK_LED3_MSK) | LED_BLINK(DK_LED4_MSK),
        LEDS_PATTERN_DONE     = LED_BLINK(DK_LED4_MSK),
        LEDS_PAIRED           = LED_ON(DK_LED3_MSK),
        LEDS_ERROR            = LED_ON(DK_ALL_LEDS_MSK)
} display_state;


/* Structures for work */
static struct k_delayed_work leds_update_work;
static struct k_work connect_work;

enum error_type {
        ERROR_NRF_CLOUD,
        ERROR_BSD_RECOVERABLE,
        ERROR_BSD_IRRECOVERABLE
};

/* Forward declaration of functions */
static void cloud_connect(struct k_work *work);
static void work_init(void);

/**@brief nRF Cloud error handler. */
void error_handler(enum error_type err_type, int err)
{
        if (err_type == ERROR_NRF_CLOUD) {
                int err;

                /* Turn off and shutdown modem */
                k_sched_lock();
                err = lte_lc_power_off();
                __ASSERT(err == 0, "lte_lc_power_off failed: %d", err);
                bsdlib_shutdown();
        }

#if !defined(CONFIG_DEBUG)
        sys_reboot(SYS_REBOOT_COLD);
#else
        u8_t led_pattern;

        switch (err_type) {
        case ERROR_NRF_CLOUD:
                /* Blinking all LEDs ON/OFF in pairs (1 and 4, 2 and 3)
                 * if there is an application error.
                 */
                led_pattern = DK_LED1_MSK | DK_LED4_MSK;
                printk("Error of type ERROR_NRF_CLOUD: %d\n", err);
                break;
        case ERROR_BSD_RECOVERABLE:
                /* Blinking all LEDs ON/OFF in pairs (1 and 3, 2 and 4)
                 * if there is a recoverable error.
                 */
                led_pattern = DK_LED1_MSK | DK_LED3_MSK;
                printk("Error of type ERROR_BSD_RECOVERABLE: %d\n", err);
                break;
        case ERROR_BSD_IRRECOVERABLE:
                /* Blinking all LEDs ON/OFF if there is an
                 * irrecoverable error.
                 */
                led_pattern = DK_ALL_LEDS_MSK;
                printk("Error of type ERROR_BSD_IRRECOVERABLE: %d\n", err);
                break;
        default:
                /* Blinking all LEDs ON/OFF in pairs (1 and 2, 3 and 4)
                 * undefined error.
                 */
                led_pattern = DK_LED1_MSK | DK_LED2_MSK;
                break;
        }

        while (true) {
                dk_set_leds(led_pattern & 0x0f);
                k_busy_wait(LEDS_ERROR_UPDATE_INTERVAL);
                dk_set_leds(~led_pattern & 0x0f);
                k_busy_wait(LEDS_ERROR_UPDATE_INTERVAL);
        }
#endif /* CONFIG_DEBUG */
}

void nrf_cloud_error_handler(int err)
{
        error_handler(ERROR_NRF_CLOUD, err);
}

/**@brief Recoverable BSD library error. */
void bsd_recoverable_error_handler(uint32_t err)
{
        error_handler(ERROR_BSD_RECOVERABLE, (int)err);
}

/**@brief Irrecoverable BSD library error. */
void bsd_irrecoverable_error_handler(uint32_t err)
{
        error_handler(ERROR_BSD_IRRECOVERABLE, (int)err);
}

/**@brief Update LEDs state. */
static void leds_update(struct k_work *work)
{
        static bool led_on;
        static u8_t current_led_on_mask;
        u8_t led_on_mask = current_led_on_mask;

        ARG_UNUSED(work);

        /* Reset LED3 and LED4. */
        led_on_mask &= ~(DK_LED3_MSK | DK_LED4_MSK);

        /* Set LED3 and LED4 to match current state. */
        led_on_mask |= LED_GET_ON(display_state);

        led_on = !led_on;
        if (led_on) {
                led_on_mask |= LED_GET_BLINK(display_state);
        } else {
                led_on_mask &= ~LED_GET_BLINK(display_state);
        }

        if (led_on_mask != current_led_on_mask) {
                dk_set_leds(led_on_mask);
                current_led_on_mask = led_on_mask;
        }

        k_delayed_work_submit(&leds_update_work, LEDS_UPDATE_INTERVAL);
}

/**@brief Callback for nRF Cloud events. */
static void cloud_event_handler(const struct nrf_cloud_evt *evt)
{
        int err;

        switch (evt->type) {

        case NRF_CLOUD_EVT_TRANSPORT_CONNECTED:
                printk("NRF_CLOUD_EVT_TRANSPORT_CONNECTED\n");
                break;
        case NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST:
                printk("NRF_CLOUD_EVT_USER_ASSOCIATION_REQUEST\n");
                //on_user_association_req(evt);
                break;
        case NRF_CLOUD_EVT_USER_ASSOCIATED:
                printk("NRF_CLOUD_EVT_USER_ASSOCIATED\n");
                break;
        case NRF_CLOUD_EVT_READY:
                printk("NRF_CLOUD_EVT_READY\n");
                display_state = LEDS_PAIRED;
                break;
        case NRF_CLOUD_EVT_SENSOR_ATTACHED:
                printk("NRF_CLOUD_EVT_SENSOR_ATTACHED\n");
                break;
        case NRF_CLOUD_EVT_SENSOR_DATA_ACK:
                printk("NRF_CLOUD_EVT_SENSOR_DATA_ACK\n");
                break;
        case NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED:
                printk("NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED\n");
                display_state = LEDS_INITIALIZING;

                /* Reconnect to nRF Cloud. */
                k_work_submit(&connect_work);
                break;
        case NRF_CLOUD_EVT_ERROR:
                printk("NRF_CLOUD_EVT_ERROR, status: %d\n", evt->status);

                display_state = LEDS_ERROR;
                nrf_cloud_error_handler(evt->status);
                break;

        case NRF_CLOUD_EVT_RX_DATA:
                printk("NRF_CLOUD_EVT_RX_DATA\n");
                break;

        case NRF_CLOUD_EVT_FOTA_DONE:
                sys_reboot(0);
                break;

        default:
                printk("Received unknown event %d\n", evt->type);
                break;
        }

}

/**@brief Initialize nRF CLoud library. */
static void cloud_init(void)
{
        const struct nrf_cloud_init_param param = {
                .event_handler = cloud_event_handler
        };

        int err = nrf_cloud_init(&param);

        __ASSERT(err == 0, "nRF Cloud library could not be initialized.");
}

/**@brief Connect to nRF Cloud, */
static void cloud_connect(struct k_work *work)
{
        int err;

        ARG_UNUSED(work);

        const struct nrf_cloud_connect_param param = {
                .sensor = NULL,
        };

        display_state = LEDS_CONNECTING;
        err = nrf_cloud_connect(&param);

        if (err) {
                printk("nrf_cloud_connect failed: %d\n", err);
                nrf_cloud_error_handler(err);
        }
        else{
                printk("nrf_cloud_connect success: %d\n", err);
        }
}

/**@brief Initializes and submits delayed work. */
static void work_init(void)
{
        k_delayed_work_init(&leds_update_work, leds_update);
        k_work_init(&connect_work, cloud_connect);
        k_delayed_work_submit(&leds_update_work, LEDS_UPDATE_INTERVAL);
}

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static void modem_configure(void)
{
        if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
                /* Do nothing, modem is already turned on and connected. */
        } else {
                int err;

                printk("Establishing LTE link (this may take some time) ...\n");
                display_state = LEDS_CONNECTING;
                err = lte_lc_init_and_connect();
                __ASSERT(err == 0, "LTE link could not be established.");
        }
}


/**@brief Initializes buttons and LEDs, using the DK buttons and LEDs
 * library.
 */
static void buttons_leds_init(void)
{
        int err;

        err = dk_leds_init();
        if (err) {
                printk("Could not initialize leds, err code: %d\n", err);
        }

        err = dk_set_leds_state(0x00, DK_ALL_LEDS_MSK);
        if (err) {
                printk("Could not set leds state, err code: %d\n", err);
        }
}

void main(void)
{
        printk("Application started\n");

        buttons_leds_init();
        ble_init();

        work_init();
        cloud_init();
        modem_configure();
        cloud_connect(NULL);

        if (IS_ENABLED(CONFIG_CLOUD_UA_CONSOLE)) {
                console_init();
        }



        while (true) {
                nrf_cloud_process();
                k_sleep(K_MSEC(10));
                k_cpu_idle();
        }
}

