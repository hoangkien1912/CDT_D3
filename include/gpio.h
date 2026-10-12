/* Giao diện GPIO dùng chung giữa phần điều khiển và phần truy cập thanh ghi.
 * File chỉ khai báo kiểu dữ liệu/hàm, không chứa máy trạng thái thực thi.
 * Include guard bên dưới ngăn khai báo lặp khi header được include nhiều lần. */
#ifndef GPIO_H
#define GPIO_H

/* Kiểu uint8_t là số nguyên không dấu 8 bit, phạm vi 0..255. */
#include <stdint.h>

/* Chỉ số 7 input, liên tiếp từ 0 đến 6 để truy cập mảng chống dội.
 * Giữ thứ tự FILL/CAP/LABEL liên tiếp vì stations_update() tính chỉ số cảm biến.
 * Đây là mã định danh chân logic, không phải các state của FSM. */
typedef enum {
    GPIO_INPUT_START = 0,      /* I0: D2 / PD2, nút khởi động. */
    GPIO_INPUT_STOP,           /* I1: D3 / PD3, nút dừng. */
    GPIO_INPUT_BOTTLE_LOWER,   /* I2: D4 / PD4, cảm biến chai dưới. */
    GPIO_INPUT_BOTTLE_UPPER,   /* I3: D5 / PD5, cảm biến chai trên. */
    GPIO_INPUT_FILL_POSITION, /* I4: D6 / PD6, chai tại trạm chiết. */
    GPIO_INPUT_CAP_POSITION,  /* I5: D7 / PD7, chai tại trạm đóng nắp. */
    GPIO_INPUT_LABEL_POSITION /* I6: D8 / PB0, chai tại trạm dán nhãn. */
} gpio_input_t;

/* Chỉ số 8 output active-high. Các giá trị phải liên tiếp từ 0 đến
 * CHECK_I2_I3 để outputs_off() duyệt và tắt đầy đủ các chân. */
typedef enum {
    GPIO_OUTPUT_CONVEYOR = 0, /* A1 / PC1: băng tải. */
    GPIO_OUTPUT_FILL_VALVE,   /* A0 / PC0: van chiết rót. */
    GPIO_OUTPUT_FILL_STOPPER, /* D13 / PB5: chặn chai khi chiết. */
    GPIO_OUTPUT_CAP_HEAD,     /* D12 / PB4: đầu đóng nắp. */
    GPIO_OUTPUT_CAP_STOPPER,  /* D11 / PB3: chặn chai khi đóng nắp. */
    GPIO_OUTPUT_LABELER,      /* D10 / PB2: bộ dán nhãn. */
    GPIO_OUTPUT_REJECT_PUSHER,/* D9 / PB1: xi lanh loại chai. */
    GPIO_OUTPUT_CHECK_I2_I3   /* A2 / PC2: LED xác nhận I2/I3. */
} gpio_output_t;

/* Cấu hình hướng chân và pull-up nội; đưa tất cả output về LOW.
 * Gọi một lần lúc khởi động trước khi đọc input hoặc điều khiển output.
 * Không có tham số, không trả về giá trị; chi tiết thanh ghi nằm ở gpio.c.
 */
void gpio_init(void);

/* input: mã chân cần đọc. Trả về logic 1 khi chân bị kéo LOW (tác động),
 * logic 0 khi chân HIGH (nhả) hoặc mã không hợp lệ. Chưa thực hiện chống dội. */
uint8_t gpio_read_input(gpio_input_t input);

/* output: mã chân cần ghi; active = 0 để tắt/LOW, khác 0 để bật/HIGH.
 * Không trả về giá trị; mã không hợp lệ không gây thay đổi GPIO.
 * Chỉ thay đổi chân chỉ định, không tự điều khiển các chân cùng trạm. */
void gpio_write_output(gpio_output_t output, uint8_t active);

#endif
