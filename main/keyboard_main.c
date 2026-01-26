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

// --- UART 配置 ---
#define TXD_PIN 17            // 发送管脚
#define RXD_PIN 18            // 接收管脚
#define UART_BUAD_RATE 115200 // 波特率
#define UART_PORT UART_NUM_1  // 使用的 UART 端口

// --- Key 配置 ---
#define KEYPRESS_INTERVAL_MS 500                                  // 触发间隔，单位毫秒
#define TIMER_INTERVAL_MS 10                                      // 定时器周期，单位毫秒
#define TICK_COUNT_MAX (KEYPRESS_INTERVAL_MS / TIMER_INTERVAL_MS) // 计算最大计数值

static uint32_t tick_counter = 0; // 计时器滴答计数器
static uint8_t current_key = 0;   // 当前按住的键码
static uint8_t current_mod = 0;   // 当前按住的修饰键

// 将 USB HID 键码转换为 ASCII 字符:
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

    // 索引计算:
    uint8_t idx = key_code - 0x04;

    // 处理 Ctrl 组合键 (仅针对 A-Z):
    if (ctrl && idx <= 25)
    {
        return idx + 1; // Ctrl+A = 0x01 ...
    }

    // 返回对应的 ASCII:
    return shift ? lut_shift[idx] : lut_plain[idx];
}

void uart_repeat_send_task(void *pvParameters)
{
    uint8_t prev_key = 0;
    while (1)
    {
        // 复制全局变量至本地变量:
        uint8_t local_key = current_key;
        uint8_t local_mod = current_mod;

        if (local_key != prev_key)
        {
            // 按键状态变化，重置计数器:
            tick_counter = 0;
        }
        if (local_key != 0)
        {
            if (tick_counter == 0)
            {
                // 转换并发送ASCII码:
                char ascii_char = usb_keycode_to_ascii(local_key, local_mod);
                if (ascii_char != 0)
                {
                    if (ascii_char >= 32 && ascii_char <= 126)
                    {
                        ESP_LOGI("UART", "Send: 0x%02X: [%c]", (uint8_t)ascii_char, ascii_char);
                    }
                    else
                    {
                        ESP_LOGI("UART", "Send: 0x%02X", (uint8_t)ascii_char);
                    }
                    // 通过UART发送:
                    uart_write_bytes(UART_NUM_1, &ascii_char, 1);
                }
            }
            tick_counter++;
            if (tick_counter >= TICK_COUNT_MAX)
            {
                tick_counter = 0;
            }
        }
        else
        {
            tick_counter = 0;
        }
        prev_key = local_key;
        // 根据配置的间隔进行延时:
        vTaskDelay(pdMS_TO_TICKS(TIMER_INTERVAL_MS));
    }
}

// 当键盘有按键动作时的回调函数:
void hid_host_keyboard_report_callback(const uint8_t *report, size_t report_len, void *arg)
{
    // 标准 HID 键盘报告格式 (8字节):
    // report[0]: 修饰键 (Ctrl, Shift, etc)
    // report[1]: 保留
    // report[2~7]: 同时按下的键码 (最多6个)
    // report[2] 是第一个按下的键的键码 (Keycode)
    if (report_len < 3)
        return;
    // 更新当前全局状态:
    current_mod = report[0];
    current_key = report[2]; // 即使是 0 (释放) 也会赋值给 current_key
    ESP_LOGI("KEYBOARD", "Key pressed: 0x%02X, mod: 0x%02X", current_key, current_mod);
}

// 设备打开后的回调
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg)
{
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT)
    {
        size_t report_read_len;
        uint8_t report[8]; // 键盘标准报告是8字节

        // 获取实际产生该事件的数据:
        esp_err_t err = hid_host_device_get_raw_input_report_data(
            hid_device_handle,
            report,
            sizeof(report),
            &report_read_len);

        if (err == ESP_OK)
        {
            hid_host_keyboard_report_callback(report, report_read_len, NULL);
        }
        else
        {
            ESP_LOGE("HID", "Failed to get input report data");
        }
    }
}

// 处理 HID 协议栈事件的回调:
void hid_host_device_event_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED)
    {
        // 发现新设备:
        hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL};
        esp_err_t err = hid_host_device_open(hid_device_handle, &dev_config);
        if (err != ESP_OK)
        {
            ESP_LOGE("App", "Failed to open HID device");
            return;
        }
        err = hid_host_device_start(hid_device_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE("App", "Failed to start HID device");
            return;
        }
        ESP_LOGI("App", "Keyboard connected and opened");
    }
}

// 初始化 UART:
void init_uart()
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BUAD_RATE,
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
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGW("App", "This is %s chip with %d CPU core(s).", CONFIG_IDF_TARGET, chip_info.cores);

    // 初始化串口
    init_uart();

    // 初始化 USB Host 栈:
    const usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    usb_host_install(&host_config);

    // 初始化 USB Host:
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .stack_size = 4096,
        .task_priority = 10,
        .callback = hid_host_device_event_callback, // 必须注册这个监听连接事件
        .callback_arg = NULL};
    hid_host_install(&hid_config);

    // 创建重复发送任务:
    xTaskCreate(uart_repeat_send_task, "uart_repeat_send_task", 4096, NULL, 10, NULL);

    ESP_LOGW("App", "System ready, waiting for USB keyboard events...");

    while (1)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}
