/*
 * Điều khiển dây chuyền bằng các máy trạng thái (FSM), không dùng delay.
 * Luồng chai: kiểm tra I2/I3 -> loại chai hoặc chiết rót -> đóng nắp -> dán nhãn.
 * Mỗi trạm có state, số yêu cầu chờ và mốc thời gian riêng; vòng lặp lần lượt
 * cập nhật các trạm nên chúng có thể hoạt động đồng thời mà không chờ nhau.
 * Chân input tác động ở mức điện 0, nhưng gpio_read_input() đảo lại thành
 * logic 1 = đang tác động. Các ký hiệu I0..I4 bên dưới đã qua chống dội.
 */
#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include "gpio.h"

#include <avr/interrupt.h>
#include <util/atomic.h>

/* Đơn vị thời gian là ms. DEBOUNCE_MS thực tế là số mẫu khác trạng thái cũ
 * liên tiếp (20 mẫu); chỉ tương đương khoảng 20 ms nếu lấy mẫu đều mỗi ms. */
#define AUTO_OFF_TIME_MS    30000UL
#define HEAD_CONFIRM_MS     200UL
#define DEBOUNCE_MS         2U
#define INPUT_COUNT         7U

typedef enum {
    /* Tất cả output đã tắt; chờ START và STOP không tác động. */
    MAIN_SYS_OFF = 0,
    /* Băng tải chạy, cho phép nhận chai; STOP hoặc cờ 30 s sẽ tắt hệ thống. */
    MAIN_SYS_ON
} main_state_t;

typedef enum {
    /* Chờ I2 hoặc I3 tác động khi hệ thống đang bật. */
    HEAD_IDLE = 0,
    /* Chờ đủ 200 ms rồi xét mức I2/I3 hiện tại để phân loại. */
    HEAD_CONFIRM,
    /* Đã phân loại; chờ cả I2 và I3 nhả để không đếm lặp cùng một chai. */
    HEAD_WAIT_CLEAR
} head_state_t;

typedef enum {
    /* Chờ yêu cầu trong queue và cảm biến vị trí (trạm loại không cần vị trí). */
    IDLE = 0,
    /* Output trạm đang bật; giữ đến khi đủ duration tương ứng. */
    DOING,
    /* Output đã tắt; chờ cảm biến nhả trước khi nhận yêu cầu tiếp theo. */
    WAIT_CLEAR
} station_state_t;

typedef enum {
    /* Xóa cờ hết giờ; chỉ bắt đầu đếm khi cả I2 và I3 không tác động. */
    TM_RESET = 0,
    /* Đếm thời gian I2/I3 cùng vắng; một cảm biến tác động sẽ hủy lần đếm. */
    TM_COUNTING,
    /* Đã đủ 30 s; giữ cờ cho main_fsm() xử lý tắt hệ thống. */
    TM_TRIGGER
} timer_state_t;

/* ISR ghi, chương trình chính đọc: volatile buộc truy cập giá trị thực.
 * Biến static mặc định bằng 0; đọc 32 bit trên AVR 8 bit cần vùng atomic. */
static volatile uint32_t system_millis;

static main_state_t    main_state   = MAIN_SYS_OFF;
static head_state_t    head_state   = HEAD_IDLE;
static timer_state_t   timer_state  = TM_RESET;

/* Mốc bắt đầu xác nhận chai và bắt đầu khoảng vắng chai, tính theo tick ms. */
static uint32_t head_started_at;
static uint32_t timer_started_at;

/* Tín hiệu hết 30 giây từ bộ đếm sang FSM bật/tắt hệ thống. */
static uint8_t flag_30s;

/* Thứ tự này cũng dùng để tra duration, chọn cảm biến và chuyển queue i + 1.
 * Các mảng bên dưới dùng cùng chỉ số i, nên không cần một struct lồng nhau. */
enum { REJECT, FILL, CAP, LABEL, STATION_COUNT };
/* Mỗi trạm có 3 giá trị riêng nhưng dùng chung chỉ số:
 * station_state[i]  = state hiện tại;
 * station_queue[i]  = số yêu cầu đang chờ;
 * station_started_at[i] = thời điểm output được bật.
 * Khởi tạo tĩnh đưa state về IDLE, queue và thời gian về 0. */
static station_state_t station_state[STATION_COUNT];
static uint8_t station_queue[STATION_COUNT];
static uint32_t station_started_at[STATION_COUNT];
/* Loại: 1 s; chiết: 5 s; đóng nắp: 2 s; dán nhãn: 2 s. */
static const uint16_t duration[STATION_COUNT] = { 1000, 5000, 2000, 2000 };

/* input_state: logic ổn định; input_counter: số mẫu liên tiếp khác logic đó.
 * last_sample_ms ngăn lấy mẫu lặp nhiều lần trong cùng một tick. */
static uint8_t input_state[INPUT_COUNT];
static uint8_t input_counter[INPUT_COUNT];
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

/* ===== Các hàm hỗ trợ xử lý trạng thái ===== */

/* Trả về ảnh chụp nhất quán của bộ đếm ms. Tạm khóa ngắt khi đọc đủ 4 byte,
 * tránh ISR tăng bộ đếm giữa chừng; sau đó khôi phục trạng thái ngắt ban đầu. */
static uint32_t millis_now(void)
{
    uint32_t now;

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        now = system_millis;
    }

    return now;
}

/* Đọc và chống dội độc lập cho 7 input, tối đa một mẫu mỗi giá trị now.
 * Mẫu giống trạng thái ổn định: xóa bộ đếm. Mẫu khác: tăng bộ đếm;
 * đủ 20 mẫu khác liên tiếp mới chấp nhận trạng thái mới (cả bật lẫn nhả).
 * Nếu vòng lặp chậm và bỏ qua tick, hàm không lấy bù mẫu đã bỏ lỡ. */
static void inputs_update(uint32_t now)
{
    uint8_t i;

    if (now == last_sample_ms) {
        return;
    }
    last_sample_ms = now;

    for (i = 0; i < INPUT_COUNT; i++) {
        uint8_t raw = gpio_read_input((gpio_input_t)i);

        if (raw == input_state[i]) {
            input_counter[i] = 0;
        } else if (++input_counter[i] >= DEBOUNCE_MS) {
            input_state[i] = raw;
            input_counter[i] = 0;
        }
    }
}

/* Thêm một yêu cầu vào bộ đếm hàng chờ qua con trỏ.
 * Bão hòa ở 255: khi đầy, bỏ yêu cầu mới để tránh tràn uint8_t về 0. */
static void queue_push(uint8_t *queue)
{
    if (*queue < 255U) {
        (*queue)++;
    }
}

/* Tắt toàn bộ 8 output, gồm băng tải, các cơ cấu và LED xác nhận.
 * Vòng lặp dựa vào enum output liên tục từ 0 đến CHECK_I2_I3. */
static void outputs_off(void)
{
    for (uint8_t i = 0; i <= GPIO_OUTPUT_CHECK_I2_I3; i++) {
        gpio_write_output((gpio_output_t)i, 0);
    }
}

/* Dừng ngay các output và đưa các FSM về trạng thái ban đầu.
 * Xóa mọi yêu cầu đang chờ; chu trình đang chạy bị hủy, không được tiếp tục.
 * Mốc thời gian phân loại và bộ đếm được ghi lại khi bắt đầu chu trình mới.
 * Trạng thái input chống dội và đồng hồ ms vẫn được giữ. */
static void system_off(void)
{
    outputs_off();
    main_state   = MAIN_SYS_OFF;
    head_state   = HEAD_IDLE;

    timer_state  = TM_RESET;
    flag_30s     = 0;
    for (uint8_t i = 0; i < STATION_COUNT; i++) {
        station_state[i] = IDLE;
        station_queue[i] = 0;
        station_started_at[i] = 0;
    }
}

/* ===== Các máy trạng thái điều khiển dây chuyền ===== */

/* FSM bật/tắt tổng, dùng trạng thái nút nhấn sau chống dội (không bắt cạnh).
 * Vì vậy giữ START khi STOP đã nhả có thể làm hệ thống khởi động lại. */
static void main_fsm(void)
{
    switch (main_state) {
    case MAIN_SYS_OFF:
        /* STOP ưu tiên: chỉ bật băng tải nếu START có và STOP không có. */
        if (I0_START && !I1_STOP) {
            gpio_write_output(GPIO_OUTPUT_CONVEYOR, 1);
            main_state = MAIN_SYS_ON;
        }
        break;

    case MAIN_SYS_ON:
        /* Dừng do người vận hành hoặc do 30 s không thấy I2/I3. */
        if (I1_STOP || flag_30s) {
            system_off();
        }
        break;
    }
}

/* FSM tự tắt sau 30 s liên tục không có tín hiệu ở I2 và I3.
 * Chỉ kiểm tra đầu vào dây chuyền, không kiểm tra trạm sau còn chai hay không.
 * Phép trừ uint32_t tính thời gian đã qua vẫn đúng khi đồng hồ quay vòng,
 * với điều kiện khoảng thời gian thực chưa vượt một chu kỳ bộ đếm. */
static void timer_fsm(uint32_t now)
{
    if (!SYS_ON) {
        /* Khi tắt hệ thống, không tích lũy thời gian vắng chai. */
        timer_state = TM_RESET;
        flag_30s = 0;
        return;
    }

    switch (timer_state) {
    case TM_RESET:
        /* Bắt đầu một khoảng đếm mới khi cả hai cảm biến đều nhả. */
        flag_30s = 0;
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
            flag_30s = 1;
            timer_state = TM_TRIGGER;
        }
        break;

    case TM_TRIGGER:
        /* Giữ nguyên cờ; main_fsm() gọi system_off() để reset FSM này. */
        break;
    }
}

/* FSM phân loại chai theo tín hiệu tại thời điểm đủ 200 ms:
 * - I2 và I3 cùng tác động: xếp yêu cầu chiết nếu hàng chờ chưa đầy.
 * - Chỉ I2 tác động: xếp yêu cầu loại.
 * - I2 không tác động: không xếp yêu cầu nào.
 * Không ghi nhớ tín hiệu từng xuất hiện trong khoảng chờ.
 * Khi tắt hệ thống, trạng thái đã được reset trước khi xử lý phân loại. */
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
            if ((I2 && I3) && station_queue[FILL] < 255U) {
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

/* Bật/tắt các cơ cấu theo chỉ số trạm; active khác 0 là bật.
 * REJECT: xi lanh loại; FILL: van + chặn chai; CAP: đầu đóng + chặn chai;
 * LABEL: bộ dán nhãn. Các cặp output được điều khiển chung một trạng thái.
 * Hàm không điều khiển băng tải hoặc LED; index ngoài 4 trạm không làm gì. */
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

/* Cập nhật lần lượt cả 4 trạm với cùng mốc now, không chờ trạm nào hoàn tất.
 * Mỗi lần gọi, mỗi trạm chỉ xử lý một case theo state ở thời điểm vào switch.
 * Queue chỉ là số lượng yêu cầu, không lưu danh tính hay vị trí từng chai. */
static void stations_update(uint32_t now)
{
    for (uint8_t i = 0; i < STATION_COUNT; i++) {
        /* Chọn cảm biến bằng từng tên rõ ràng để dễ đối chiếu sơ đồ dây:
         * REJECT không có cảm biến vị trí riêng nên dùng I2;
         * ba trạm còn lại dùng lần lượt I4, I5, I6. */
        uint8_t sensor = 0;
        if (i == REJECT) {
            sensor = I2;
        } else if (i == FILL) {
            sensor = I4;
        } else if (i == CAP) {
            sensor = input_state[GPIO_INPUT_CAP_POSITION];
        } else if (i == LABEL) {
            sensor = input_state[GPIO_INPUT_LABEL_POSITION];
        }
        switch (station_state[i]) {
        case IDLE:
            /* Có yêu cầu thì trạm loại chạy ngay; 3 trạm sau phải thấy chai
             * ở cảm biến vị trí. Lấy một yêu cầu và ghi mốc bắt đầu bật. */
            if (SYS_ON && station_queue[i] && (i == REJECT || sensor)) {
                station_queue[i]--;
                station_output(i, 1);
                station_started_at[i] = now;
                station_state[i] = DOING;
            }
            break;
        case DOING:
            /* Giữ output đến đủ thời gian, kể cả khi cảm biến đã nhả.
             * Chiết xong thêm queue đóng nắp; đóng nắp xong thêm queue dán.
             * Trạm kế tiếp có thể nhận ngay trong lượt for này nếu đủ điều kiện. */
            if ((uint32_t)(now - station_started_at[i]) >= duration[i]) {
                station_output(i, 0);
                if (i == FILL || i == CAP) {
                    queue_push(&station_queue[i + 1]);
                }
                station_state[i] = WAIT_CLEAR;
            }
            break;
        case WAIT_CLEAR:
            /* Phải thấy cảm biến nhả mới về IDLE; việc bắt đầu yêu cầu tiếp
             * chỉ diễn ra ở lần gọi stations_update() sau. */
            if (!sensor) {
                station_state[i] = IDLE;
            }
            break;
        }
    }
}

/* ===== Chương trình chính: setup một lần, cập nhật FSM liên tục ===== */

/* Điểm vào chương trình: cấu hình GPIO, tắt output, tạo tick rồi bật ngắt.
 * Vòng lặp chạy liên tục; mọi khoảng chờ dùng mốc thời gian thay cho delay. */
int main(void)
{
    hardware_setup();

    while (1) {
        /* Một ảnh chụp thời gian dùng chung cho tất cả xử lý trong lượt này. */
        uint32_t now = millis_now();

        inputs_update(now);

        /* Timer chạy trước để main_fsm() nhận cờ hết 30 s ngay cùng lượt.
         * Main chạy trước các trạm để lệnh STOP tắt/reset chúng trước cập nhật. */
        timer_fsm(now);
        main_fsm();

        head_fsm(now);
        /* I4 tắt LED A2. Nếu I4 chưa tác động, LED giữ giá trị đã ghi dù
         * I2/I3 đã nhả; system_off() cũng tắt LED khi dừng hệ thống. */
        if (I4) {
            gpio_write_output(GPIO_OUTPUT_CHECK_I2_I3, 0);
        }
        stations_update(now);
    }
}

/* ===== Định nghĩa setup phần cứng và ngắt đồng hồ ===== */

/* Khởi tạo một lần trước vòng lặp; chỉ bật ngắt khi phần cứng đã sẵn sàng. */
static void hardware_setup(void)
{
    gpio_init();
    outputs_off();
    timer1_init();
    sei();
}

/* Cấu hình Timer1 làm đồng hồ ms riêng của chương trình:
 * - CTC (WGM12): đếm từ 0 đến OCR1A rồi tự về 0.
 * - Prescaler 64: tại F_CPU = 16 MHz, timer chạy 250 kHz.
 * - OCR1A = 249: 250 nhịp tạo một ngắt, tức 1 ms.
 * Hàm chỉ bật nguồn ngắt Timer1; hardware_setup() bật ngắt toàn cục sau đó. */
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

/* Ngắt so sánh A của Timer1: mỗi 1 ms chỉ tăng bộ đếm để ISR ngắn gọn.
 * Bộ đếm uint32_t tự quay về 0 sau khoảng 49,7 ngày. */
ISR(TIMER1_COMPA_vect)
{
    system_millis++;
}
