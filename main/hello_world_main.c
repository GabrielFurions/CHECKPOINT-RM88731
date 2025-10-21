/* main.c
   Sistema multitarefa FreeRTOS no ESP32
   Autor: {Gabriel Pessoa Aires-RM:88731}
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

#define IDENT "{Gabriel Pessoa Aires-RM:88731}"

static const char *TAG = "MULTI_MODULE";

// Configurações
#define QUEUE_LENGTH        5
#define QUEUE_ITEM_SIZE     sizeof(int)
#define GENERATOR_PERIOD_MS 200
#define RECEIVER_WAIT_MS    1000
#define SUPERVISOR_PERIOD_MS 2000
#define WDT_TIMEOUT_S       6

// Flags (EventGroup)
#define BIT_GENERATOR_OK    (1 << 0)
#define BIT_RECEIVER_OK     (1 << 1)
#define BIT_SUPERVISOR_OK   (1 << 2)
#define BIT_RECOVERY        (1 << 3)

// Handles globais
static QueueHandle_t data_queue = NULL;
static EventGroupHandle_t status_events = NULL;
static TaskHandle_t generator_task_handle = NULL;
static TaskHandle_t receiver_task_handle  = NULL;
static TaskHandle_t supervisor_task_handle = NULL;

/* -------------------------
   Utilitários WDT
   ------------------------- */
static void register_task_wdt_or_log(TaskHandle_t task)
{
    BaseType_t res = esp_task_wdt_add(task);
    if (res != ESP_OK)
        ESP_LOGW(TAG, "%s [WDT] Falha ao registrar task (%d)", IDENT, (int)res);
}

/* -------------------------
   Gerador de Dados
   ------------------------- */
static void generator_task(void *arg)
{
    ESP_LOGI(TAG, "%s [GERADOR] Iniciando tarefa de geração...", IDENT);
    register_task_wdt_or_log(NULL);
    xEventGroupSetBits(status_events, BIT_GENERATOR_OK);

    int counter = 0;
    TickType_t period = pdMS_TO_TICKS(GENERATOR_PERIOD_MS);

    while (1) {
        int value = counter++;

        if (xQueueSendToBack(data_queue, &value, 0) == pdPASS) {
            ESP_LOGI(TAG, "%s [FILA] Dado enviado com sucesso! Valor=%d", IDENT, value);
        } else {
            ESP_LOGW(TAG, "%s [FILA] Fila cheia. Dado %d descartado!", IDENT, value);
        }

        esp_task_wdt_reset();
        xEventGroupSetBits(status_events, BIT_GENERATOR_OK);
        vTaskDelay(period);
    }
}

/* -------------------------
   Recuperação do gerador
   ------------------------- */
static void attempt_recover_generator()
{
    ESP_LOGW(TAG, "%s [RECEPTOR] Tentando recuperar gerador...", IDENT);
    xEventGroupSetBits(status_events, BIT_RECOVERY);

    if (generator_task_handle != NULL) {
        vTaskResume(generator_task_handle);
        ESP_LOGI(TAG, "%s [RECUPERACAO] Gerador reativado com sucesso!", IDENT);
    } else {
        ESP_LOGW(TAG, "%s [RECUPERACAO] Gerador ausente. Recriando...", IDENT);
        BaseType_t res = xTaskCreatePinnedToCore(
            generator_task, "generator_task", 4096, NULL,
            tskIDLE_PRIORITY + 2, &generator_task_handle, tskNO_AFFINITY
        );
        if (res == pdPASS)
            ESP_LOGI(TAG, "%s [RECUPERACAO] Gerador recriado com sucesso!", IDENT);
        else
            ESP_LOGE(TAG, "%s [RECUPERACAO] Falha ao recriar gerador!", IDENT);
    }

    xEventGroupClearBits(status_events, BIT_RECOVERY);
}

/* -------------------------
   Receptor de Dados
   ------------------------- */
static void receiver_task(void *arg)
{
    ESP_LOGI(TAG, "%s [RECEPTOR] Iniciando tarefa de recepção...", IDENT);
    register_task_wdt_or_log(NULL);
    xEventGroupSetBits(status_events, BIT_RECEIVER_OK);

    int miss_count = 0;

    while (1) {
        int buffer;
        int *p_val = NULL;

        if (xQueueReceive(data_queue, &buffer, pdMS_TO_TICKS(RECEIVER_WAIT_MS)) == pdPASS) {
            p_val = malloc(sizeof(int));
            if (p_val) {
                *p_val = buffer;
                ESP_LOGI(TAG, "%s [RECEPTOR] Dado recebido: %d (malloc=%p)", IDENT, *p_val, (void*)p_val);
                vTaskDelay(pdMS_TO_TICKS(50));
                free(p_val);
            } else {
                ESP_LOGE(TAG, "%s [ERRO] Falha em malloc() para valor %d", IDENT, buffer);
            }

            miss_count = 0;
            xEventGroupSetBits(status_events, BIT_RECEIVER_OK);
        } else {
            miss_count++;
            ESP_LOGW(TAG, "%s [TIMEOUT] Nenhum dado recebido (%d tentativas)", IDENT, miss_count);

            if (miss_count == 1) {
                ESP_LOGW(TAG, "%s [AVISO] Verificando fila de dados...", IDENT);
            } else if (miss_count == 3) {
                ESP_LOGW(TAG, "%s [RECUPERACAO] Tentando recuperar o gerador...", IDENT);
                attempt_recover_generator();
            } else if (miss_count >= 5) {
                ESP_LOGE(TAG, "%s [FALHA] Falha crítica. Reiniciando dispositivo...", IDENT);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else {
                xEventGroupClearBits(status_events, BIT_RECEIVER_OK);
            }
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* -------------------------
   Supervisor
   ------------------------- */
static void supervisor_task(void *arg)
{
    ESP_LOGI(TAG, "%s [SUPERVISOR] Iniciando monitoramento...", IDENT);
    register_task_wdt_or_log(NULL);
    xEventGroupSetBits(status_events, BIT_SUPERVISOR_OK);

    while (1) {
        EventBits_t bits = xEventGroupGetBits(status_events);
        bool g_ok = bits & BIT_GENERATOR_OK;
        bool r_ok = bits & BIT_RECEIVER_OK;
        bool s_ok = bits & BIT_SUPERVISOR_OK;
        bool rec = bits & BIT_RECOVERY;

        ESP_LOGI(TAG, "%s [STATUS] Gerador:%s | Receptor:%s | Supervisor:%s | Recuperacao:%s",
                 IDENT,
                 g_ok ? "OK" : "NOK",
                 r_ok ? "OK" : "NOK",
                 s_ok ? "OK" : "NOK",
                 rec ? "SIM" : "NAO");

        if (!g_ok && !rec) {
            ESP_LOGW(TAG, "%s [SUPERVISOR] Gerador inativo. Tentando restaurar...", IDENT);
            if (generator_task_handle != NULL)
                vTaskResume(generator_task_handle);
            else
                xTaskCreate(generator_task, "generator_task", 4096, NULL, tskIDLE_PRIORITY + 2, &generator_task_handle);
            xEventGroupSetBits(status_events, BIT_RECOVERY);
            vTaskDelay(pdMS_TO_TICKS(100));
            xEventGroupClearBits(status_events, BIT_RECOVERY);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));
    }
}

/* -------------------------
   Função principal
   ------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "%s [SISTEMA] Inicializando multitarefa FreeRTOS + WDT", IDENT);

    // ✅ Correção para ESP-IDF v5.x
    const esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_S * 1000,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&wdt_config));

    data_queue = xQueueCreate(QUEUE_LENGTH, QUEUE_ITEM_SIZE);
    status_events = xEventGroupCreate();

    xTaskCreate(generator_task, "generator_task", 4096, NULL, tskIDLE_PRIORITY + 2, &generator_task_handle);
    xTaskCreate(receiver_task, "receiver_task", 6144, NULL, tskIDLE_PRIORITY + 3, &receiver_task_handle);
    xTaskCreate(supervisor_task, "supervisor_task", 4096, NULL, tskIDLE_PRIORITY + 1, &supervisor_task_handle);

    ESP_LOGI(TAG, "%s [SISTEMA] Tarefas criadas com sucesso!", IDENT);
}