#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include "gpio.h"

#include <avr/interrupt.h>
#include <util/atomic.h>

#define AUTO_OFF_TIME_MS    30000UL
#define HEAD_CONFIRM_MS     200UL
#define DEBOUNCE_SAMPLES    2U

typedef enum {
    MAIN_SYS_OFF = 0,
    MAIN_SYS_ON
} main_state_t;

typedef enum {
    HEAD_IDLE = 0,
    HEAD_CONFIRM,
    HEAD_WAIT_CLEAR
} head_state_t;

typedef enum {
    STATION_IDLE = 0,
    STATION_RUNNING,
    STATION_WAIT_CLEAR
} station_state_t;

typedef enum {
    TM_RESET = 0,
    TM_COUNTING,
    TM_TRIGGER
} timer_state_t;

static volatile uint32_t system_millis;

static main_state_t    main_state   = MAIN_SYS_OFF;
static head_state_t    head_state   = HEAD_IDLE;
static timer_state_t   timer_state  = TM_RESET;

/* Mốc bắt đầu xác nhận chai và bắt đầu khoảng vắng chai, tính theo tick ms. */
static uint32_t head_started_at;
static uint32_t timer_started_at;

static uint8_t auto_off_requested;

/* Giữ FILL, CAP, LABEL liên tiếp để chuyển hàng đợi sang trạm i + 1. */
enum { REJECT, FILL, CAP, LABEL, STATION_COUNT };
/* Các mảng dùng chung chỉ số trạm; khởi tạo tĩnh về 0 (STATION_IDLE). */
static station_state_t station_state[STATION_COUNT];
static uint8_t station_queue[STATION_COUNT];
static uint32_t station_started_at[STATION_COUNT];
/* Loại: 1 s; chiết: 5 s; đóng nắp: 2 s; dán nhãn: 2 s. */
static const uint16_t station_duration_ms[STATION_COUNT] = { 1000, 5000, 2000, 2000 };

/* Cùng thứ tự REJECT, FILL, CAP, LABEL; I2 dùng để chờ nhả sau khi loại. */
static const gpio_input_t station_sensor[STATION_COUNT] = {
    GPIO_INPUT_BOTTLE_LOWER,
    GPIO_INPUT_FILL_POSITION,
    GPIO_INPUT_CAP_POSITION,
    GPIO_INPUT_LABEL_POSITION
};

static uint8_t input_state[GPIO_INPUT_COUNT];
static uint8_t input_counter[GPIO_INPUT_COUNT];
static uint32_t last_sample_ms;

#define I0_START   (input_state[GPIO_INPUT_START])
#define I1_STOP    (input_state[GPIO_INPUT_STOP])
#define I2         (input_state[GPIO_INPUT_BOTTLE_LOWER])
#define I3         (input_state[GPIO_INPUT_BOTTLE_UPPER])
#define I4         (input_state[GPIO_INPUT_FILL_POSITION])

#define SYS_ON     (main_state == MAIN_SYS_ON)

/* ===== Khai báo setup: phần định nghĩa nằm sau main ===== */
static void hardware_setup(void);
static void timer1_init(void);

static uint32_t millis_now(void)
{
    uint32_t now;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        now = system_millis;
    }

    return now;
}

/* Xác nhận thay đổi sau hai mẫu ở hai tick khác nhau, không lấy bù tick. */
static void inputs_update(uint32_t now)
{
    uint8_t i;

    if (now == last_sample_ms) {
        return;
    }
    last_sample_ms = now;

    for (i = 0; i < GPIO_INPUT_COUNT; i++) {
        uint8_t raw = gpio_read_input((gpio_input_t)i);

        if (raw == input_state[i]) {
            input_counter[i] = 0;
        } else if (++input_counter[i] >= DEBOUNCE_SAMPLES) {
            input_state[i] = raw;
            input_counter[i] = 0;
        }
    }
}

static void queue_push(uint8_t *queue)
{
    if (*queue < UINT8_MAX) {
        (*queue)++;
    }
}

static void outputs_off(void)
{
    for (uint8_t i = 0; i < GPIO_OUTPUT_COUNT; i++) {
        gpio_write_output((gpio_output_t)i, 0);
    }
}

static void system_off(void)
{
    outputs_off();
    main_state = MAIN_SYS_OFF;
    head_state = HEAD_IDLE;

    timer_state = TM_RESET;
    auto_off_requested = 0;
    for (uint8_t i = 0; i < STATION_COUNT; i++) {
        station_state[i] = STATION_IDLE;
        station_queue[i] = 0;
        station_started_at[i] = 0;
    }
}

/* ===== Các máy trạng thái điều khiển dây chuyền ===== */

static void main_fsm(void)
{
    switch (main_state) {
    case MAIN_SYS_OFF:
        if (I0_START && !I1_STOP) {
            gpio_write_output(GPIO_OUTPUT_CONVEYOR, 1);
            main_state = MAIN_SYS_ON;
        }
        break;

    case MAIN_SYS_ON:
        if (I1_STOP || auto_off_requested) {
            system_off();
        }
        break;
    }
}

static void timer_fsm(uint32_t now)
{
    if (!SYS_ON) {
        /* Khi tắt hệ thống, không tích lũy thời gian vắng chai. */
        timer_state = TM_RESET;
        auto_off_requested = 0;
        return;
    }

    switch (timer_state) {
    case TM_RESET:
        /* Bắt đầu một khoảng đếm mới khi cả hai cảm biến đều nhả. */
        auto_off_requested = 0;
        if (!I2 && !I3) {
            timer_started_at = now;
            timer_state = TM_COUNTING;
        }
        break;

    case TM_COUNTING:
        /* Có chai: hủy đếm; vẫn vắng và đủ 30 s: chốt cờ tắt hệ thống. */
        if (I2 || I3) {
            timer_state = TM_RESET;
        } else if ((uint32_t)(now - timer_started_at) >= AUTO_OFF_TIME_MS) {
            auto_off_requested = 1;
            timer_state = TM_TRIGGER;
        }
        break;

    case TM_TRIGGER:
        /* Giữ nguyên cờ; main_fsm() gọi system_off() để reset FSM này. */
        break;
    }
}

static void head_fsm(uint32_t now)
{
    switch (head_state) {
    case HEAD_IDLE:
        /* Bắt đầu chờ 200 ms trước khi xét tín hiệu phân loại. */
        if (SYS_ON && (I2 || I3)) {
            head_started_at = now;
            head_state = HEAD_CONFIRM;
        }
        break;

    case HEAD_CONFIRM:
        if ((uint32_t)(now - head_started_at) >= HEAD_CONFIRM_MS) {
            /* Chai đạt nhưng queue FILL đầy sẽ không được xếp hoặc chuyển sang loại.
             * LED chỉ bật khi xếp FILL thành công và I4 chưa tác động. */
            if ((I2 && I3) && station_queue[FILL] < UINT8_MAX) {
                queue_push(&station_queue[FILL]);
                gpio_write_output(GPIO_OUTPUT_CHECK_I2_I3, !I4);
            } else if (I2 && !I3) {
                queue_push(&station_queue[REJECT]);
            }
            head_state = HEAD_WAIT_CLEAR;
        }
        break;

    case HEAD_WAIT_CLEAR:
        /* Chờ vùng kiểm tra trống hoàn toàn trước khi nhận chai tiếp. */
        if (!I2 && !I3) {
            head_state = HEAD_IDLE;
        }
        break;
    }
}

static void station_output(uint8_t index, uint8_t active)
{
    switch (index) {
    case REJECT:
        gpio_write_output(GPIO_OUTPUT_REJECT_PUSHER, active);
        break;
    case FILL:
        gpio_write_output(GPIO_OUTPUT_FILL_VALVE, active);
        gpio_write_output(GPIO_OUTPUT_FILL_STOPPER, active);
        break;
    case CAP:
        gpio_write_output(GPIO_OUTPUT_CAP_HEAD, active);
        gpio_write_output(GPIO_OUTPUT_CAP_STOPPER, active);
        break;
    case LABEL:
        gpio_write_output(GPIO_OUTPUT_LABELER, active);
        break;
    }
}

static void stations_update(uint32_t now)
{
    for (uint8_t i = 0; i < STATION_COUNT; i++) {
        uint8_t sensor = input_state[station_sensor[i]];

        switch (station_state[i]) {
        case STATION_IDLE:
            if (SYS_ON && station_queue[i] && (i == REJECT || sensor)) {
                station_queue[i]--;
                station_output(i, 1);
                station_started_at[i] = now;
                station_state[i] = STATION_RUNNING;
            }
            break;
        case STATION_RUNNING:
            if ((uint32_t)(now - station_started_at[i]) >= station_duration_ms[i]) {
                station_output(i, 0);
                if (i == FILL || i == CAP) {
                    queue_push(&station_queue[i + 1]);
                }
                station_state[i] = STATION_WAIT_CLEAR;
            }
            break;
        case STATION_WAIT_CLEAR:
            /* Phải thấy cảm biến nhả mới về STATION_IDLE; việc bắt đầu yêu cầu tiếp
             * chỉ diễn ra ở lần gọi stations_update() sau. */
            if (!sensor) {
                station_state[i] = STATION_IDLE;
            }
            break;
        }
    }
}

int main(void)
{
    hardware_setup();

    while (1) {
        uint32_t now = millis_now();

        inputs_update(now);

        timer_fsm(now);
        main_fsm();

        head_fsm(now);
        if (I4) {
            gpio_write_output(GPIO_OUTPUT_CHECK_I2_I3, 0);
        }
        stations_update(now);
    }
}

static void hardware_setup(void)
{
    gpio_init();
    outputs_off();
    timer1_init();
    sei();
}

static void timer1_init(void)
{
    TCCR1A = 0;
    TCCR1B = _BV(WGM12);
    TCNT1 = 0;
    OCR1A = (uint16_t)(F_CPU / 64UL / 1000UL - 1UL);
    /* Cờ AVR được xóa bằng cách ghi 1; bỏ cờ so sánh cũ trước khi bật ngắt. */
    TIFR1 = _BV(OCF1A);
    TIMSK1 = _BV(OCIE1A);
    TCCR1B |= _BV(CS11) | _BV(CS10);
}

ISR(TIMER1_COMPA_vect)
{
    system_millis++;
}
