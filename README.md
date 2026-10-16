# Hệ thống dây chuyền chiết rót và đóng gói

Firmware chạy trên Arduino Uno (ATmega328P), điều khiển một dây chuyền gồm:

- Băng tải.
- Phát hiện chai bằng hai cảm biến I2/I3.
- Phân loại chai lỗi.
- Chiết rót, đóng nắp và dán nhãn theo hàng đợi.
- Tự dừng khi cả I2 và I3 không tác động liên tục 30 giây.

## Công nghệ

- PlatformIO
- Arduino framework
- Board `uno`
- Vi điều khiển ATmega328P, xung nhịp 16 MHz
- Ngôn ngữ C

## Sơ đồ chân

### Input

Các input sử dụng pull-up nội. Cảm biến hoặc nút nhấn được xem là tác động khi kéo chân xuống GND.

| Tín hiệu | Chân Arduino | Chân ATmega328P |
| --- | --- | --- |
| START | D2 | PD2 |
| STOP | D3 | PD3 |
| I2 - cảm biến chai dưới | D4 | PD4 |
| I3 - cảm biến chai trên | D5 | PD5 |
| I4 - vị trí chiết rót | D6 | PD6 |
| I5 - vị trí đóng nắp | D7 | PD7 |
| I6 - vị trí dán nhãn | D8 | PB0 |

### Output

Các output là active-high: ghi mức HIGH để bật, LOW để tắt.

| Chức năng | Chân Arduino | Chân ATmega328P |
| --- | --- | --- |
| Băng tải | A1 | PC1 |
| Van chiết rót | A0 | PC0 |
| Chặn chai tại trạm chiết | D13 | PB5 |
| Đầu đóng nắp | D12 | PB4 |
| Chặn chai tại trạm đóng nắp | D11 | PB3 |
| Bộ dán nhãn | D10 | PB2 |
| Xi lanh loại chai | D9 | PB1 |
| LED xác nhận I2/I3 | A2 | PC2 |

## Luồng hoạt động

1. Nhấn START khi STOP chưa tác động để bật băng tải.
2. Toàn bộ 7 input được chống dội bằng hai mẫu thay đổi liên tiếp ở hai tick khác nhau (thường khoảng 2 ms).
3. Chai được xác nhận sau 200 ms:
   - I2 và I3 cùng tác động: đưa vào hàng đợi chiết rót.
   - Chỉ I2 tác động: đưa vào hàng đợi loại chai.
4. Các trạm hoạt động độc lập: chiết → đóng nắp → dán nhãn chuyển yêu cầu qua hàng đợi và chờ cảm biến vị trí. Trạm loại chạy ngay khi có yêu cầu, sau đó chờ I2 nhả.
5. Thời gian xử lý: loại chai 1 giây, chiết rót 5 giây, đóng nắp 2 giây và dán nhãn 2 giây.
6. STOP hoặc I2/I3 cùng nhả liên tục 30 giây sẽ tắt toàn bộ output, xóa hàng đợi và reset các máy trạng thái.

## Cấu trúc mã nguồn

Mã C được tổ chức trong đúng ba file:

- [src/main.c](src/main.c): các kiểu dữ liệu, chống dội, máy trạng thái hệ thống và từng trạm, Timer1, vòng lặp đọc/ghi GPIO.
- [src/gpio.c](src/gpio.c): bảng ánh xạ chân, cấu hình pull-up và truy cập thanh ghi.
- [include/gpio.h](include/gpio.h): enum input/output và giao diện GPIO.

[platformio.ini](platformio.ini) cấu hình môi trường `uno`. Dù dùng Arduino framework để build, firmware định nghĩa `main` riêng, không dùng `setup/loop` hay `millis` của Arduino.

### Tổ chức dữ liệu

- `controller_t`: trạng thái hệ thống, bộ nhận diện chai, timer vắng chai, mảng trạm và output.
- `station_runtime_t`: trạng thái, số yêu cầu chờ và thời điểm bắt đầu của một trạm.
- Trạng thái và mốc thời gian phân loại/vắng chai đặt trực tiếp trong `controller_t`, không tạo typedef trung gian chỉ dùng một lần. Mỗi trạm vẫn có dữ liệu riêng để chạy song song.
- `input_filter_t`: giá trị đã lọc, bộ đếm mẫu thay đổi và tick lấy mẫu gần nhất.

Các instance nằm cục bộ trong `main`; các hàm xử lý nhận con trỏ tới struct. Bộ lọc đọc trực tiếp GPIO khi sang tick mới, không cần buffer input thô. Chỉ `system_millis` là biến trạng thái toàn cục thay đổi được, dùng chung với ISR. Các bảng cấu hình là `static const`.

### Thời gian và thứ tự xử lý

Timer1 chạy CTC, prescaler 64, OCR1A = 249 tại 16 MHz để tạo tick 1 ms. ISR chỉ tăng bộ đếm; việc đọc bộ đếm 32 bit dùng atomic block. Hiệu thời gian dùng phép trừ unsigned để xử lý vòng tràn.

Mỗi vòng lặp: đọc input → chống dội → timer vắng chai → START/STOP → phân loại → tắt LED khi I4 tác động → xử lý trạm → ghi output. Không dùng delay chặn. Mỗi trạm chuyển `IDLE → RUNNING → WAITING_FOR_CLEAR → IDLE`.

## Các hành vi cần biết

- Sau khi I2 hoặc I3 tác động, chờ 200 ms rồi xét trạng thái tại thời điểm đó; không yêu cầu cả hai giữ liên tục 200 ms.
- Chỉ I3 tác động, hoặc cả hai đã nhả khi hết 200 ms: không tạo yêu cầu.
- Sau phân loại phải chờ cả I2/I3 nhả mới nhận chai tiếp theo.
- Trạm loại bắt đầu ngay khi có yêu cầu; I2 chỉ dùng để chờ nhả sau khi kết thúc loại.
- Trạm chiết/đóng nắp/dán nhãn cần cả yêu cầu chờ và cảm biến vị trí; các trạm có thể chạy đồng thời.
- Hoàn tất chiết tạo yêu cầu đóng nắp; hoàn tất đóng nắp tạo yêu cầu dán nhãn.
- Hàng đợi mỗi trạm tối đa 255 yêu cầu, không quay vòng; yêu cầu mới bị bỏ qua khi đầy. Chai đạt không tự chuyển sang loại khi hàng đợi chiết đầy.
- LED A2 bật khi xếp hàng chiết thành công và I4 chưa tác động; tắt khi I4 tác động hoặc hệ thống dừng.
- Timer 30 giây chỉ quan sát I2/I3, không quan sát chai ở các trạm sau.
- START kích hoạt theo mức: nếu vẫn giữ START sau khi dừng, hệ thống có thể chạy lại khi STOP nhả hoặc ở vòng lặp sau lần tự tắt.
- STOP đi qua bộ chống dội như các input khác. Dừng sẽ bỏ tiến trình và hàng đợi, không tiếp tục chu kỳ cũ.
- Không lấy bù mẫu chống dội nếu vòng lặp bỏ qua tick. Số RAM tĩnh của báo cáo build chưa bao gồm stack chứa các struct cục bộ.

## Build và upload

Cài PlatformIO Core hoặc extension PlatformIO IDE trong VS Code, mở thư mục chứa [platformio.ini](platformio.ini), rồi chạy:

```text
pio run -e uno
pio run -e uno --target upload
```

Nếu không tìm thấy `pio` trong PowerShell, dùng PlatformIO Terminal hoặc đường dẫn mặc định trên Windows:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e uno
```

Có thể chọn cổng nạp bằng `--upload-port COM3` (thay bằng cổng thực tế). Firmware không xuất log UART nên Serial Monitor không hiển thị trạng thái.

## Kiểm tra trên board

1. Reset: mọi output LOW; START bật băng tải; START và STOP cùng tác động thì STOP ưu tiên.
2. I2/I3 cùng tác động qua thời điểm xác nhận: có yêu cầu chiết; I4 kích hoạt van và chặn chai 5 giây.
3. I5 kích hoạt đóng nắp 2 giây sau chiết; I6 kích hoạt dán nhãn 2 giây sau đóng nắp.
4. Chỉ I2 tác động tại lúc xác nhận: xi lanh loại bật 1 giây; giữ I2 không tạo thêm yêu cầu cho cùng lần phát hiện.
5. Nhấn STOP khi trạm đang chạy: mọi output tắt và hàng đợi bị xóa.
6. Nhả I2/I3 liên tục 30 giây khi hệ thống chạy: tự tắt, kể cả còn chai ở trạm sau.

Build thành công chỉ xác nhận khả năng biên dịch/liên kết, không thay thế kiểm thử timing và tín hiệu trên phần cứng. Project không còn chương trình test C riêng sau khi gom về ba file.

## Lưu ý phần cứng

- Không nối trực tiếp tải công suất vào chân GPIO; dùng mạch driver phù hợp.
- Input pull-up nội yêu cầu tín hiệu kéo xuống GND khi tác động; output dùng active-high.
- Timer1 được firmware sử dụng riêng; không dùng thêm chức năng/thư viện chiếm Timer1.
