/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"

#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "lcd_panel.st7789";

static esp_err_t panel_st7789_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7789_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data);
static esp_err_t panel_st7789_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st7789_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7789_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st7789_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st7789_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_cal; // save surrent value of LCD_CMD_COLMOD register
} st7789_panel_t;

esp_err_t
esp_lcd_new_panel_st7789(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                         esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    st7789_panel_t *st7789 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    st7789 = calloc(1, sizeof(st7789_panel_t));
    ESP_GOTO_ON_FALSE(st7789, ESP_ERR_NO_MEM, err, TAG, "no mem for st7789 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ENDIAN_RGB:
        st7789->madctl_val = 0;
        break;
    case LCD_RGB_ENDIAN_BGR:
        st7789->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7789->colmod_cal = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        st7789->colmod_cal = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7789->io = io;
    st7789->fb_bits_per_pixel = fb_bits_per_pixel;
    st7789->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7789->reset_level = panel_dev_config->flags.reset_active_high;
    st7789->base.del = panel_st7789_del;
    st7789->base.reset = panel_st7789_reset;
    st7789->base.init = panel_st7789_init;
    st7789->base.draw_bitmap = panel_st7789_draw_bitmap;
    st7789->base.invert_color = panel_st7789_invert_color;
    st7789->base.set_gap = panel_st7789_set_gap;
    st7789->base.mirror = panel_st7789_mirror;
    st7789->base.swap_xy = panel_st7789_swap_xy;
    st7789->base.disp_on_off = panel_st7789_disp_on_off;
    *ret_panel = &(st7789->base);
    ESP_LOGD(TAG, "new st7789 panel @%p", st7789);

    return ESP_OK;

err:
    if (st7789) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7789);
    }
    return ret;
}

static esp_err_t panel_st7789_del(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);

    if (st7789->reset_gpio_num >= 0) {
        gpio_reset_pin(st7789->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st7789 panel @%p", st7789);
    free(st7789);
    return ESP_OK;
}

static esp_err_t panel_st7789_reset(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;

    // perform hardware reset
    if (st7789->reset_gpio_num >= 0) {
        gpio_set_level(st7789->reset_gpio_num, st7789->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7789->reset_gpio_num, !st7789->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG,
                            "io tx param LCD_CMD_SWRESET failed");
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5m before sending new command
    }

    return ESP_OK;
}

static esp_err_t panel_st7789_init(esp_lcd_panel_t *panel)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG,
                        "io tx param LCD_CMD_SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789->madctl_val,
    }, 1), TAG, "io tx param LCD_CMD_MADCTL failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        st7789->colmod_cal,
    }, 1), TAG, "io tx param LCD_CMD_COLMOD failed");

#define ANALOG_GAMMA
//#define GAMMA_CURVE		1

#ifdef ANALOG_GAMMA

#ifdef GAMMA_CURVE

    esp_lcd_panel_io_tx_param(io, LCD_CMD_GAMSET, (uint8_t[]) {
    	(1 << GAMMA_CURVE),
    }, 1);

#else
    esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t[]) {
//    	0x70, 0x2c, 0x2e, 0x15, 0x10, 0x09, 0x48, 0x33, 0x53, 0x0b, 0x19, 0x19, 0x20, 0x25	// ST7789V default
//    	0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30	// ER-TFT024IPS-3 buydisplay.com
//    	0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19	// newhavendisplay.com
//    	0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0e, 0x12, 0x14, 0x17	// https://github.com/birdtechstep/tinydrm/blob/master/st7789.c
//    	0xd0, 0x06, 0x0b, 0x0a, 0x09, 0x05, 0x2e, 0x43, 0x44, 0x09, 0x16, 0x15, 0x23, 0x27	// EA TFT020-23AI
//    	0xf0, 0x04, 0x08, 0x07, 0x08, 0x05, 0x39, 0x43, 0x50, 0x38, 0x18, 0x18, 0x30, 0x34	// optimized
///    	0xf0, 0x07, 0x10, 0x10, 0x10, 0x28, 0x40, 0x43, 0x55, 0x7, 0x10, 0x14, 0x30, 0x33
///		0xf0, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40, 0x43, 0x55, 0x07, 0x10, 0x14, 0x30, 0x33
///    	0xf0, 0x04, 0x08, 0x06, 0x06, 0x28, 0x40, 0x43, 0x50, 0x38, 0x12, 0x12, 0x28, 0x30
////		0xf0, 0x04, 0x08, 0x06, 0x06, 0x28, 0x40, 0x43, 0x60, 0x1c, 0x1c, 0x18, 0x35, 0x36	// good, dark
		0xf0, 0x04, 0x08, 0x06, 0x08, 0x28, 0x40, 0x43, 0x60, 0x1e, 0x1c, 0x18, 0x35, 0x36	// high contrast
    }, 14);
    esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t[]) {
//    	0x70, 0x2c, 0x2e, 0x15, 0x10, 0x09, 0x48, 0x33, 0x53, 0x0b, 0x19, 0x19, 0x20, 0x25	// ST7789V default
//    	0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32
//    	0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19
//    	0xd0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31, 0x54, 0x47, 0x0e, 0x1c, 0x17, 0x1b, 0x1e
//    	0xd0, 0x06, 0x0b, 0x09, 0x08, 0x06, 0x2e, 0x44, 0x44, 0x3a, 0x15, 0x15, 0x23, 0x26
//    	0xf0, 0x04, 0x08, 0x07, 0x08, 0x05, 0x39, 0x43, 0x50, 0x38, 0x18, 0x18, 0x30, 0x34
///    	0xf0, 0x07, 0x10, 0x10, 0x10, 0x28, 0x40, 0x43, 0x55, 0x7, 0x10, 0x14, 0x30, 0x33
///    	0xf0, 0x00, 0x00, 0x00, 0x00, 0x20, 0x40, 0x43, 0x55, 0x07, 0x10, 0x14, 0x30, 0x33
///		0xf0, 0x04, 0x08, 0x06, 0x06, 0x28, 0x40, 0x43, 0x50, 0x38, 0x12, 0x12, 0x28, 0x30
////		0xf0, 0x04, 0x08, 0x06, 0x06, 0x28, 0x40, 0x43, 0x60, 0x1c, 0x1c, 0x18, 0x35, 0x36	// good, dark
		0xf0, 0x04, 0x08, 0x06, 0x08, 0x28, 0x40, 0x43, 0x60, 0x1e, 0x1c, 0x18, 0x35, 0x36	// high contrast
    }, 14);
#endif

#else	// digital gamma

#define LCD_GAMMA 045
#define LCD_GAMMA_RED
//#define LCD_GAMMA_BLUE


#ifdef LCD_GAMMA_RED
    esp_lcd_panel_io_tx_param(io, 0xE2, (uint8_t[]) {		// digital gamma red
#if LCD_GAMMA == 020
    	0, 111, 128, 139, 147, 154, 159, 164, 169, 173, 176, 180, 183, 186, 189, 191,
		194, 196, 198, 201, 203, 205, 207, 208, 210, 212, 214, 215, 217, 218, 220, 221,
		223, 224, 225, 227, 228, 229, 230, 232, 233, 234, 235, 236, 237, 238, 239, 240,
		242, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 253, 254, 255
#elif LCD_GAMMA == 045
		0,  40,  54,  65,  74,  82,  89,  95, 101, 106, 111, 116, 121, 125, 130, 134,
		138, 141, 145, 149, 152, 156, 159, 162, 165, 168, 171, 174, 177, 180, 183, 185,
		188, 191, 193, 196, 198, 201, 203, 206, 208, 210, 212, 215, 217, 219, 221, 224,
		226, 228, 230, 232, 234, 236, 238, 240, 242, 244, 246, 248, 249, 251, 253, 255
#elif LCD_GAMMA == 070
		0,  14,  23,  30,  37,  43,  49,  55,  60,  65,  70,  75,  80,  84,  89,  93,
		98, 102, 106, 110, 114, 118, 122, 126, 130, 134, 137, 141, 145, 148, 152, 155,
		159, 162, 166, 169, 172, 176, 179, 182, 186, 189, 192, 195, 198, 201, 205, 208,
		211, 214, 217, 220, 223, 226, 229, 232, 235, 238, 241, 244, 246, 249, 252, 255
#elif LCD_GAMMA == 180
		0,   0,   1,   1,   2,   3,   4,   5,   6,   8,   9,  11,  13,  15,  17,  19,
		22,  24,  27,  29,  32,  35,  38,  42,  45,  48,  52,  55,  59,  63,  67,  71,
		75,  80,  84,  89,  93,  98, 103, 108, 113, 118, 123, 128, 134, 139, 145, 150,
		156, 162, 168, 174, 181, 187, 193, 200, 206, 213, 220, 227, 234, 241, 248, 255
#elif LCD_GAMMA == 300
		0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   2,   2,   3,   3,
		4,   5,   6,   7,   8,   9,  11,  12,  14,  16,  18,  20,  22,  25,  28,  30,
		33,  37,  40,  44,  48,  52,  56,  60,  65,  70,  76,  81,  87,  93,  99, 106,
		113, 120, 127, 135, 143, 152, 161, 170, 179, 189, 199, 209, 220, 231, 243, 255
#endif
    }, 64);
#endif

#ifdef LCD_GAMMA_BLUE
    esp_lcd_panel_io_tx_param(io, 0xE3, (uint8_t[]) {		// digital gamma blue
#if LCD_GAMMA == 020
    	0, 111, 128, 139, 147, 154, 159, 164, 169, 173, 176, 180, 183, 186, 189, 191,
		194, 196, 198, 201, 203, 205, 207, 208, 210, 212, 214, 215, 217, 218, 220, 221,
		223, 224, 225, 227, 228, 229, 230, 232, 233, 234, 235, 236, 237, 238, 239, 240,
		242, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 253, 254, 255
#elif LCD_GAMMA == 045
		0,  40,  54,  65,  74,  82,  89,  95, 101, 106, 111, 116, 121, 125, 130, 134,
		138, 141, 145, 149, 152, 156, 159, 162, 165, 168, 171, 174, 177, 180, 183, 185,
		188, 191, 193, 196, 198, 201, 203, 206, 208, 210, 212, 215, 217, 219, 221, 224,
		226, 228, 230, 232, 234, 236, 238, 240, 242, 244, 246, 248, 249, 251, 253, 255
#elif LCD_GAMMA == 070
		0,  14,  23,  30,  37,  43,  49,  55,  60,  65,  70,  75,  80,  84,  89,  93,
		98, 102, 106, 110, 114, 118, 122, 126, 130, 134, 137, 141, 145, 148, 152, 155,
		159, 162, 166, 169, 172, 176, 179, 182, 186, 189, 192, 195, 198, 201, 205, 208,
		211, 214, 217, 220, 223, 226, 229, 232, 235, 238, 241, 244, 246, 249, 252, 255
#elif LCD_GAMMA == 180
		0,   0,   1,   1,   2,   3,   4,   5,   6,   8,   9,  11,  13,  15,  17,  19,
		22,  24,  27,  29,  32,  35,  38,  42,  45,  48,  52,  55,  59,  63,  67,  71,
		75,  80,  84,  89,  93,  98, 103, 108, 113, 118, 123, 128, 134, 139, 145, 150,
		156, 162, 168, 174, 181, 187, 193, 200, 206, 213, 220, 227, 234, 241, 248, 255
#elif LCD_GAMMA == 300
		0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   2,   2,   3,   3,
		4,   5,   6,   7,   8,   9,  11,  12,  14,  16,  18,  20,  22,  25,  28,  30,
		33,  37,  40,  44,  48,  52,  56,  60,  65,  70,  76,  81,  87,  93,  99, 106,
		113, 120, 127, 135, 143, 152, 161, 170, 179, 189, 199, 209, 220, 231, 243, 255
#endif
    }, 64);
#endif

#if defined(LCD_GAMMA_RED) || defined(LCD_GAMMA_BLUE)
    esp_lcd_panel_io_tx_param(io, 0xBA, (uint8_t[]) {		// digital gamma enable
    	0x04
    }, 1);
#endif

#endif
    return ESP_OK;
}

static esp_err_t panel_st7789_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = st7789->io;

    x_start += st7789->x_gap;
    x_end += st7789->x_gap;
    y_start += st7789->y_gap;
    y_end += st7789->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "io tx param LCD_CMD_CASET failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "io tx param LCD_CMD_RASET failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * st7789->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "io tx color failed");

    return ESP_OK;
}

static esp_err_t panel_st7789_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param LCD_CMD_INVON/LCD_CMD_INVOFF failed");
    return ESP_OK;
}

static esp_err_t panel_st7789_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    if (mirror_x) {
        st7789->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st7789->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st7789->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st7789->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789->madctl_val
    }, 1), TAG, "io tx param LCD_CMD_MADCTL failed");
    return ESP_OK;
}

static esp_err_t panel_st7789_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    if (swap_axes) {
        st7789->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st7789->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        st7789->madctl_val
    }, 1), TAG, "io tx param LCD_CMD_MADCTL failed");
    return ESP_OK;
}

static esp_err_t panel_st7789_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    st7789->x_gap = x_gap;
    st7789->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st7789_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7789_panel_t *st7789 = __containerof(panel, st7789_panel_t, base);
    esp_lcd_panel_io_handle_t io = st7789->io;
    int command = 0;
    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param LCD_CMD_DISPON/LCD_CMD_DISPOFF failed");
    return ESP_OK;
}
