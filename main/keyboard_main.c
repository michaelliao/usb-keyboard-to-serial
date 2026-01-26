/**
 * SPDX-License-Identifier: GPLv3
 */
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#define TXD_PIN 17
#define RXD_PIN 18
#define UART_PORT UART_NUM_1

// --- 配置部分 ---
#define KEYPRESS_INTERVAL_MS 500                                  // 触发间隔，单位毫秒
#define TIMER_INTERVAL_MS 10                                      // 定时器周期，单位毫秒
#define TICK_COUNT_MAX (KEYPRESS_INTERVAL_MS / TIMER_INTERVAL_MS) // 计算最大计数值

static uint32_t tick_counter = 0; // 计时器滴答计数器
static uint8_t current_key = 0;   // 当前按住的键码
static uint8_t current_mod = 0;   // 当前按住的修饰键

/**
 * 将 USB HID 键码转换为 ASCII 码，增加 Ctrl 处理
 */
char usb_keycode_to_ascii(uint8_t key_code, uint8_t modifier)
{
    // 映射表 (索引 0 对应 Keycode 0x04)
    // 注意：\\ 是反斜杠，\" 是双引号
    // 位置：A-Z, 1-0, Enter, Esc, Backspace, Tab, Space, - = [ ] \ (non) ; ' ` , . /
    const static char *lut_shift = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1B\b\t _+{}| :\"~<>?";
    const static char *lut_plain = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1B\b\t -=[]\\ ;'`,./";

    // 基础偏移量：HID 键码是从 0x04 (字母A) 开始的
    if (key_code < 0x04 || key_code > 0x38)
    {
        return 0;
    }

    bool shift = (modifier & 0x02) || (modifier & 0x20);
    bool ctrl = (modifier & 0x01) || (modifier & 0x10);

    // 索引计算
    uint8_t idx = key_code - 0x04;

    // 1. 处理 Ctrl 组合键 (仅针对 A-Z)
    if (ctrl && idx <= 25)
    {
        return idx + 1; // Ctrl+A = 0x01 ...
    }

    // 2. 返回对应的 ASCII
    return shift ? lut_shift[idx] : lut_plain[idx];
}

void uart_repeat_send_task(void *pvParameters)
{
    while (1)
    {
        if (current_key != 0)
        {
            if (tick_counter == 0)
            {
                // 转换并发送
                char ascii_char = usb_keycode_to_ascii(current_key, current_mod);
                if (ascii_char != 0)
                {
                    ESP_LOGI("UART", "Auto-Repeat Send: %c", ascii_char);
                    uart_write_bytes(UART_NUM_1, &ascii_char, 1);
                }
            }
            tick_counter++;
            if (tick_counter >= TICK_COUNT_MAX)
            {
                tick_counter = 0;
            }
        }
        // 根据配置的间隔进行延时
        vTaskDelay(pdMS_TO_TICKS(TIMER_INTERVAL_MS));
    }
}

// 当键盘有按键动作时的回调函数
void hid_host_keyboard_report_callback(const uint8_t *report, size_t report_len, void *arg)
{
    /* 标准 HID 键盘报告格式 (8字节):
       report[0]: 修饰键 (Ctrl, Shift, etc)
       report[1]: 保留
       report[2]~report[7]: 同时按下的键码 (最多6个)
    */
    // report[2] 是第一个按下的键的键码 (Keycode)
    if (report_len < 3)
        return;
    static uint8_t last_key = 0;
    // 更新当前全局状态:
    current_mod = report[0];
    current_key = report[2]; // 即使是 0 (释放) 也会赋值给 current_key
    ESP_LOGI("KEYBOARD", "Key pressed: 0x%02X, mod: 0x%02X", current_key, current_mod);
    if (last_key != current_key)
    {
        tick_counter = 0; // 重置计时器
    }
    last_key = current_key;
}

// 设备打开后的回调
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event, void *arg)
{
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT)
    {
        uint8_t report[8];
        size_t report_read_len;
        hid_host_device_get_raw_input_report_data(hid_device_handle, report, 8, &report_read_len);
        hid_host_keyboard_report_callback(report, report_read_len, NULL);
    }
}

// 处理 HID 协议栈事件的回调
void hid_host_device_event_callback(hid_host_device_handle_t hid_device_handle,
                                    const hid_host_driver_event_t event,
                                    void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED)
    {
        // 发现新设备，配置它
        hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL};
        hid_host_device_open(hid_device_handle, &dev_config);
        ESP_LOGI("App", "Keyboard connected and opened");
    }
}

void init_uart()
{
    const uart_config_t uart_config = {
        .baud_rate = 115200, // 与 FPGA 的波特率保持一致
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, 1024 * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI("UART", "UART %d initialized ok, TXD_PIN = %d, RXD_PIN = %d", UART_PORT, TXD_PIN, RXD_PIN);
}

void app_main(void)
{
    ESP_LOGW("App", "Start USB keyboard to serial...");

    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGW("App", "This is %s chip with %d CPU core(s).", CONFIG_IDF_TARGET, chip_info.cores);

    // 初始化串口
    init_uart();

    const usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    usb_host_install(&host_config);

    // 初始化 USB Host
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .stack_size = 4096,
        .task_priority = 10,
        .callback = hid_host_device_event_callback, // 必须注册这个监听连接事件
        .callback_arg = NULL};
    hid_host_install(&hid_config);

    xTaskCreate(uart_repeat_send_task, "uart_repeat_send_task", 4096, NULL, 10, NULL);

    ESP_LOGW("App", "System ready, waiting for USB keyboard events...");

    while (1)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}
