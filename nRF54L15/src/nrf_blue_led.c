#include "nrf_blue_led.h"

nrf_blue_led_action_t nrf_blue_led_policy(nrf_verticality_state_t state)
{
    switch (state) {
    case NRF_VERTICALITY_ACCEPTED:
        return NRF_BLUE_LED_ON;
    case NRF_VERTICALITY_STABILIZING:
    case NRF_VERTICALITY_TILTED:
        return NRF_BLUE_LED_BLINK_2HZ;
    case NRF_VERTICALITY_FAULT:
    default:
        return NRF_BLUE_LED_OFF;
    }
}
