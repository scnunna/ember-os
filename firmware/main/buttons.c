// BOOT button (GPIO0): short = back / screen off, double = start/stop a meeting recording, HOLD = talk to Amber while
// held (release sends).
// POWER button (AXP2101 PWRON): click = screen on/off (polled every 200 ms), hold 6 s = device off/on (the PMU does it).
// Interrupt-driven: the task sleeps forever until the button goes down, polls at 10 ms only while a press or a
// double-click window is in progress, then sleeps again. The same pin wakes the chip from light sleep.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "app.h"

static TaskHandle_t s_task;

static void IRAM_ATTR btn_isr(void *a)
{
    gpio_intr_disable(GPIO_NUM_0);              // level interrupt: mask until the task has handled the press
    BaseType_t w = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &w);
    portYIELD_FROM_ISR(w);
}

static void btn_task(void *arg)
{
    bool was = false, pending = false, holding = false;
    int64_t t_down = 0, t_short = 0;
    while (1) {
        bool idle = !was && !pending && gpio_get_level(GPIO_NUM_0);
        if (idle) {
            gpio_intr_enable(GPIO_NUM_0);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else vTaskDelay(pdMS_TO_TICKS(10));
        int64_t now = esp_timer_get_time() / 1000;
        bool down = !gpio_get_level(GPIO_NUM_0);
        if (down && !was) t_down = now;
        // held for 450 ms: Amber starts listening right away (no need to wait for the release)
        if (down && was && !holding && now - t_down >= 450) { holding = true; pending = false; launcher_btn_hold(true); }
        if (!down && was) {
            int64_t dur = now - t_down;
            if (holding) { holding = false; launcher_btn_hold(false); }
            else if (dur > 30) {
                if (pending && now - t_short < 400) { pending = false; launcher_btn_double(); }
                else { pending = true; t_short = now; }
            }
        }
        if (pending && !down && now - t_short >= 400) { pending = false; launcher_btn_short(); }
        was = down;
    }
}

#include "hw.h"
static void pwr_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (hw_pwrkey_short() == 1) { printf("BTN power click\n"); launcher_btn_long(); }   // screen on/off
    }
}

void buttons_start(void)
{
    xTaskCreate(pwr_task, "pwrkey", 3072, NULL, 4, NULL);
    gpio_config_t io = {.pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    xTaskCreate(btn_task, "btn", 4096, NULL, 4, &s_task);
    gpio_install_isr_service(0);                // already installed by the touch driver: harmless error
    gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);   // also sets the interrupt type to low level
    esp_sleep_enable_gpio_wakeup();
    gpio_intr_disable(GPIO_NUM_0);
    gpio_isr_handler_add(GPIO_NUM_0, btn_isr, NULL);
}
