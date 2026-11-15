// BOOT button (GPIO0): short = back / screen off, double = talk to Amber, triple = start/stop a meeting recording,
// long = screen on/off.
// Interrupt-driven: the task sleeps forever until the button goes down, polls at 10 ms only while a press or a
// double-click window is in progress, then sleeps again. The same pin wakes the chip from light sleep.
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
    bool was = false, pending = false;
    int presses = 0;
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
        if (!down && was) {
            int64_t dur = now - t_down;
            if (dur >= 800) { pending = false; presses = 0; launcher_btn_long(); }
            else if (dur > 30) {
                // count quick presses; act 400 ms after the last one (so a double can still become a triple)
                if (pending && now - t_short < 400) presses++; else presses = 1;
                pending = true; t_short = now;
                if (presses >= 3) { pending = false; presses = 0; launcher_btn_triple(); }
            }
        }
        if (pending && !down && now - t_short >= 400) {
            pending = false;
            if (presses == 2) launcher_btn_double(); else launcher_btn_short();
            presses = 0;
        }
        was = down;
    }
}

void buttons_start(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    xTaskCreate(btn_task, "btn", 4096, NULL, 4, &s_task);
    gpio_install_isr_service(0);                // already installed by the touch driver: harmless error
    gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);   // also sets the interrupt type to low level
    esp_sleep_enable_gpio_wakeup();
    gpio_intr_disable(GPIO_NUM_0);
    gpio_isr_handler_add(GPIO_NUM_0, btn_isr, NULL);
}
