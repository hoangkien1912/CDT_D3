#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

typedef enum {
    GPIO_INPUT_START = 0,
    GPIO_INPUT_STOP,
    GPIO_INPUT_BOTTLE_LOWER,
    GPIO_INPUT_BOTTLE_UPPER,
    GPIO_INPUT_FILL_POSITION,
    GPIO_INPUT_CAP_POSITION,
    GPIO_INPUT_LABEL_POSITION,
    GPIO_INPUT_COUNT
} gpio_input_t;

typedef enum {
    GPIO_OUTPUT_CONVEYOR = 0,
    GPIO_OUTPUT_FILL_VALVE,
    GPIO_OUTPUT_FILL_STOPPER,
    GPIO_OUTPUT_CAP_HEAD,
    GPIO_OUTPUT_CAP_STOPPER,
    GPIO_OUTPUT_LABELER,
    GPIO_OUTPUT_REJECT_PUSHER,
    GPIO_OUTPUT_CHECK_I2_I3,
    GPIO_OUTPUT_COUNT
} gpio_output_t;

void gpio_init(void);
/* Input có pull-up: trả về 1 khi chân LOW, chưa chống dội. */
uint8_t gpio_read_input(gpio_input_t input);
/* Output active-high: active khác 0 ghi HIGH, bằng 0 ghi LOW. */
void gpio_write_output(gpio_output_t output, uint8_t active);

#endif
