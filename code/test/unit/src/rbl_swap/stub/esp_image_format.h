/* host test stub (rbl_swap): the SDK's esp_image_format.h, what espurna_rbl.c takes of it */
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

typedef struct {
    uint32_t start_addr;
    uint32_t image_len;
} esp_image_metadata_t;

typedef enum {
    ESP_IMAGE_VERIFY,
    ESP_IMAGE_VERIFY_SILENT,
    ESP_IMAGE_LOAD,
} esp_image_load_mode_t;

esp_err_t esp_image_load(esp_image_load_mode_t mode, const esp_partition_pos_t* part, esp_image_metadata_t* data);
