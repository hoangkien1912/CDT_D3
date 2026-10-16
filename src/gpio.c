#include "gpio.h"
#include <avr/io.h>

typedef struct {
    volatile uint8_t *pin_register;
    uint8_t mask;
} gpio_input_pin_t;

typedef struct {
    volatile uint8_t *port_register;
    uint8_t mask;
} gpio_output_pin_t;

static const gpio_input_pin_t input_pins[GPIO_INPUT_COUNT] = {
    { &PIND, _BV(PD2) },
    { &PIND, _BV(PD3) },
    { &PIND, _BV(PD4) },
    { &PIND, _BV(PD5) },
    { &PIND, _BV(PD6) },
    { &PIND, _BV(PD7) },
    { &PINB, _BV(PB0) }
};

static const gpio_output_pin_t output_pins[GPIO_OUTPUT_COUNT] = {
    { &PORTC, _BV(PC1) },
    { &PORTC, _BV(PC0) },
    { &PORTB, _BV(PB5) },
    { &PORTB, _BV(PB4) },
    { &PORTB, _BV(PB3) },
    { &PORTB, _BV(PB2) },
    { &PORTB, _BV(PB1) },
    { &PORTC, _BV(PC2) }
};

void gpio_init(void)
{
    const uint8_t input_port_d_mask = _BV(PD2) | _BV(PD3) | _BV(PD4) |
                                      _BV(PD5) | _BV(PD6) | _BV(PD7);
    const uint8_t input_port_b_mask = _BV(PB0);
    const uint8_t output_port_b_mask = _BV(PB1) | _BV(PB2) | _BV(PB3) |
                                       _BV(PB4) | _BV(PB5);
    const uint8_t output_port_c_mask = _BV(PC0) | _BV(PC1) | _BV(PC2);

    DDRD &= (uint8_t)~input_port_d_mask;
    PORTD |= input_port_d_mask;

    DDRB &= (uint8_t)~input_port_b_mask;
    PORTB |= input_port_b_mask;

    PORTB &= (uint8_t)~output_port_b_mask;
    DDRB |= output_port_b_mask;

    PORTC &= (uint8_t)~output_port_c_mask;
    DDRC |= output_port_c_mask;
}

uint8_t gpio_read_input(gpio_input_t input)
{
    if (input >= GPIO_INPUT_COUNT) {
        return 0;
    }

    return (uint8_t)!(*input_pins[input].pin_register & input_pins[input].mask);
}

void gpio_write_output(gpio_output_t output, uint8_t active)
{
    if (output >= GPIO_OUTPUT_COUNT) {
        return;
    }

    if (active) {
        *output_pins[output].port_register |= output_pins[output].mask;
    } else {
        *output_pins[output].port_register &= (uint8_t)~output_pins[output].mask;
    }
}
