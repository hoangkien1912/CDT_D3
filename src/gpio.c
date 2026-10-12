/* Lớp truy cập phần cứng GPIO của ATmega328P/Arduino Uno.
 * File này không có FSM; nhiệm vụ là ánh xạ tên input/output sang chân chip.
 * DDRx: 0 = input, 1 = output. PINx: đọc mức chân thực tế.
 * PORTx: với output là mức xuất; với input là bật/tắt pull-up nội.
 * _BV(n) tạo mặt nạ một bit (1 << n). OR bật bit, AND với mặt nạ đảo tắt bit,
 * nhờ đó chỉ thay đổi chân được chọn, giữ nguyên các chân khác trên port. */
#include "gpio.h"

#include <avr/io.h>

/* Khởi tạo 7 input có pull-up và 8 output ở mức thấp (tắt).
 * Khi công tắc/cảm biến kéo input xuống GND, chương trình hiểu là tác động.
 * Các phép mặt nạ giữ nguyên cấu hình chân ngoài nhóm được sử dụng. */
void gpio_init(void)
{
    /* DDRD & 0x03: xóa bit 2..7 thành input, giữ PD0/PD1 (UART).
     * PORTD | 0xFC: bật pull-up PD2..PD7 = D2..D7 = I0..I5.
     * DDRB & 0xFE và PORTB | 0x01: PB0 = D8 = I6 có pull-up. */
    DDRD  &= 0x03;
    PORTD |= 0xFC;
    DDRB  &= 0xFE;
    PORTB |= 0x01;

    /* Ghi latch PORT thấp trước khi chuyển DDR sang output để chân bắt đầu
     * ở mức tắt khi đổi hướng từ input sang output.
     * 0xC1/0x3E: xóa latch rồi bật hướng output cho PB1..PB5 (D9..D13).
     * 0xF8/0x07: tương tự cho PC0..PC2 (A0..A2). */
    PORTB &= 0xC1;
    DDRB  |= 0x3E;
    PORTC &= 0xF8;
    DDRC  |= 0x07;
}

/* Đọc một input theo tên logic, trả về 1 = tác động, 0 = không tác động.
 * Đọc trực tiếp thanh ghi PIN, chưa chống dội; main.c xử lý lọc sau đó.
 * pin_is_high = 1 khi điện áp chân cao; dấu ! ở cuối đảo cực tính active-low.
 * Giá trị enum không hợp lệ được xem là không tác động (trả về 0). */
uint8_t gpio_read_input(gpio_input_t input)
{
    uint8_t pin_is_high;

    switch (input) {
    case GPIO_INPUT_START:
        /* I0 / nút START: D2 = PD2. */
        pin_is_high = (PIND & _BV(PD2)) != 0;
        break;
    case GPIO_INPUT_STOP:
        /* I1 / nút STOP: D3 = PD3. */
        pin_is_high = (PIND & _BV(PD3)) != 0;
        break;
    case GPIO_INPUT_BOTTLE_LOWER:
        /* I2 / cảm biến chai dưới: D4 = PD4. */
        pin_is_high = (PIND & _BV(PD4)) != 0;
        break;
    case GPIO_INPUT_BOTTLE_UPPER:
        /* I3 / cảm biến chai trên: D5 = PD5. */
        pin_is_high = (PIND & _BV(PD5)) != 0;
        break;
    case GPIO_INPUT_FILL_POSITION:
        /* I4 / vị trí chiết rót: D6 = PD6. */
        pin_is_high = (PIND & _BV(PD6)) != 0;
        break;
    case GPIO_INPUT_CAP_POSITION:
        /* I5 / vị trí đóng nắp: D7 = PD7. */
        pin_is_high = (PIND & _BV(PD7)) != 0;
        break;
    case GPIO_INPUT_LABEL_POSITION:
        /* I6 / vị trí dán nhãn: D8 = PB0, khác port với 6 input đầu. */
        pin_is_high = (PINB & _BV(PB0)) != 0;
        break;
    default:
        return 0;
    }

    return (uint8_t)!pin_is_high;
}

/* Ghi một output theo tên logic: active khác 0 xuất HIGH, bằng 0 xuất LOW.
 * Output dùng cực tính active-high; hàm chỉ ghi mức, không giữ thời gian.
 * Ép mặt nạ đảo về uint8_t để thao tác đúng độ rộng thanh ghi 8 bit.
 * Không có case phù hợp thì không thay đổi chân nào. */
void gpio_write_output(gpio_output_t output, uint8_t active)
{
    switch (output) {
    case GPIO_OUTPUT_CONVEYOR:
        /* Băng tải: A1 = PC1. */
        if (active) PORTC |= _BV(PC1); else PORTC &= (uint8_t)~_BV(PC1);
        break;
    case GPIO_OUTPUT_FILL_VALVE:
        /* Van chiết rót: A0 = PC0. */
        if (active) PORTC |= _BV(PC0); else PORTC &= (uint8_t)~_BV(PC0);
        break;
    case GPIO_OUTPUT_FILL_STOPPER:
        /* Chặn chai trạm chiết: D13 = PB5. */
        if (active) PORTB |= _BV(PB5); else PORTB &= (uint8_t)~_BV(PB5);
        break;
    case GPIO_OUTPUT_CAP_HEAD:
        /* Đầu đóng nắp: D12 = PB4. */
        if (active) PORTB |= _BV(PB4); else PORTB &= (uint8_t)~_BV(PB4);
        break;
    case GPIO_OUTPUT_CAP_STOPPER:
        /* Chặn chai trạm đóng nắp: D11 = PB3. */
        if (active) PORTB |= _BV(PB3); else PORTB &= (uint8_t)~_BV(PB3);
        break;
    case GPIO_OUTPUT_LABELER:
        /* Bộ dán nhãn: D10 = PB2. */
        if (active) PORTB |= _BV(PB2); else PORTB &= (uint8_t)~_BV(PB2);
        break;
    case GPIO_OUTPUT_REJECT_PUSHER:
        /* Xi lanh đẩy chai loại: D9 = PB1. */
        if (active) PORTB |= _BV(PB1); else PORTB &= (uint8_t)~_BV(PB1);
        break;
    case GPIO_OUTPUT_CHECK_I2_I3:
        /* LED báo xác nhận chai bằng I2/I3: A2 = PC2. */
        if (active) PORTC |= _BV(PC2); else PORTC &= (uint8_t)~_BV(PC2);
        break;

    default:
        break;
    }
}
