/**
 * koth-timer.cpp
 * Albert Phan 2026-08-31
 * This is a koth timer featuring an LED rope and 2 main input buttons.
 * 
 * Stretch goals
 * - Speaker
 * - Configurable times
 *
 * Uses WS2812 PIO code and color blending logic inspired by the auto-feeder project.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <math.h>
#include <algorithm>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"
#include "hardware/i2c.h"

#include "pico-oled/pico-oled.hpp"
#include "pico-oled/gfx_font.h"
#include "pico-oled/font/press_start_2p.h"
#include "pico-oled/font/too_simple.h"


extern "C" {
#include "pico/bootrom.h"
}

#define DISPLAY_I2C_ADDR _u(0x3C) //_u(0x3C)
#define DISPLAY_WIDTH _u(128)
#define DISPLAY_HEIGHT _u(64)

// Pins
#define WS2812_PIN 3
#define BLUE_BUTTON_PIN 2
#define BOOT_BUTTON_PIN 16
#define RED_BUTTON_PIN 6
#define BUZZER_PIN 7
#define THROTTLE_ADC_GPIO 26
#define THROTTLE_ADC_CHANNEL 0
#define NUM_LEDS 70 // 150 is the led rope but it's too big
#define ADC_REFERENCE_VOLTAGE 3.3f

// Game Stuff
#define DEFAULT_WIN_TIME 5 // Time to hold to win round
#define DEFAULT_ROUND_TIME 10 // Time for round to end
#define OVERTIME_MS 10000 // Time losing team has to cap during overtime

#define FLASH_TIME_MS 500 // Time between flashing
#define OVERTIME_FLASH_TIME_MS 150 // Faster flash interval for the overtime timer

// LED 
#define PATTERN_BRIGHT_LEDS 6
#define PATTERN_DIM_LEDS 4
#define PATTERN_VERY_DIM_LEDS 2
#define PATTERN_PERIOD (PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS + PATTERN_VERY_DIM_LEDS)
#define PATTERN_SPEED 1.5f //
#define PATTERN_SPEED_MIN 0.2f // TESTING with POT
#define PATTERN_SPEED_MAX 1.5f // TESTING with POT
#define PATTERN_SPEED_WINNING 0.1f // Slower pattern for the team that is ahead
#define ROUND_TIME_LED_EXTRA_LENGTH 1 // amount of extra leds (half) from Round time led

// These define when the patterns are the fastest acceleration or deceleration patterns
#define THROTTLE_MAX_VOLTAGE 1.65f // Remember to convert to 3.3 V range not 5.0V
#define REGEN_VOLTAGE_START 0.561f // Remember to convert to 3.3 V range not 5.0V
#define REGEN_VOLTAGE_END 0.330f   // Remember to convert to 3.3 V range not 5.0V
#define THROTTLE_DEADBAND_V 0.12f // .18 V for 5V range

// Max hue is 6*255 = 1530
// Multiply HUE degrees by 4.25 to get HUE value
#define HUE_FACTOR 4.25
#define RED_TEAM_HUE 0
#define RED_TEAM_HUE_SATURATION 240
#define BLUE_TEAM_HUE 240
#define BLUE_TEAM_HUE_SATURATION 240
#define ROUND_TIMER_HUE 120
#define ROUND_TIMER_HUE_SATURATION 240
#define OVERTIME_TIMER_HUE 120  //
#define OVERTIME_TIMER_HUE_SATURATION 240
#define RED_TEAM_HUE_CONVERTED (RED_TEAM_HUE * HUE_FACTOR)
#define BLUE_TEAM_HUE_CONVERTED (BLUE_TEAM_HUE * HUE_FACTOR)
#define ROUND_TIMER_HUE_CONVERTED (ROUND_TIMER_HUE * HUE_FACTOR)// Green (approx 120 degrees)
#define OVERTIME_TIMER_HUE_CONVERTED (OVERTIME_TIMER_HUE * HUE_FACTOR) // Purple (approx 300 degrees)

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t) r << 8) | ((uint32_t) g << 16) | b;
}

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

static inline bool button_pressed(uint gpio) {
    return !gpio_get(gpio);
}

// Buzzer
enum class BeepPattern {
    NONE,
    ONE_HZ,
    TWO_HZ,
    THREE_HZ,
    DOUBLE_ONE_HZ,
    GAME_START,
    GAME_OVER,
    OVERTIME
};

struct BuzzerState {
    BeepPattern pattern = BeepPattern::NONE;
    uint32_t next_change_ms = 0;
    uint8_t beep_count = 0;
    bool on = false;
};

static BuzzerState buzzer;

static void buzzer_output(bool on) {
    gpio_put(BUZZER_PIN, on);
}

// Repeating beep patterns
static void start_beep_pattern(BeepPattern pattern, uint32_t now_ms) {

    // Already running this pattern
    if (buzzer.pattern == pattern) {
        return;
    }

    buzzer.pattern = pattern;
    buzzer.beep_count = 0;
    buzzer.on = false;
    buzzer.next_change_ms = now_ms;

    buzzer_output(false);
}

// For one shot patterns
static void play_beep_pattern_once(BeepPattern pattern, uint32_t now_ms) {

    buzzer.pattern = pattern;
    buzzer.beep_count = 0;
    buzzer.on = false;
    buzzer.next_change_ms = now_ms;

    buzzer_output(false);
}

static void stop_buzzer() {
    if (buzzer.pattern == BeepPattern::NONE) {
        return;
    }
    buzzer.pattern = BeepPattern::NONE;
    buzzer.on = false;
    buzzer_output(false);
}

static void update_buzzer(uint32_t now_ms,uint32_t overtime_remaining_ms) {
    static uint32_t overtime_max_ms = OVERTIME_MS;
    if (buzzer.pattern == BeepPattern::NONE) {
        return;
    }

    if (now_ms < buzzer.next_change_ms) {
        return;
    }

    switch (buzzer.pattern) {

        case BeepPattern::ONE_HZ:
            buzzer.on = !buzzer.on;
            buzzer_output(buzzer.on);

            if (buzzer.on) {
                buzzer.next_change_ms = now_ms + 100;
            } else {
                buzzer.next_change_ms = now_ms + 900;
            }
            break;


        case BeepPattern::TWO_HZ:
            buzzer.on = !buzzer.on;
            buzzer_output(buzzer.on);

            if (buzzer.on) {
                buzzer.next_change_ms = now_ms + 100;
            } else {
                buzzer.next_change_ms = now_ms + 400;
            }
            break;


        case BeepPattern::THREE_HZ:
            buzzer.on = !buzzer.on;
            buzzer_output(buzzer.on);

            if (buzzer.on) {
                buzzer.next_change_ms = now_ms + 100;
            } else {
                buzzer.next_change_ms = now_ms + 233;
            }
            break;


        case BeepPattern::DOUBLE_ONE_HZ:

            buzzer.on = !buzzer.on;
            buzzer_output(buzzer.on);

            if (buzzer.on) {
                buzzer.next_change_ms = now_ms + 100;
            } else {
                buzzer.beep_count++;

                if (buzzer.beep_count < 2) {
                    // Short gap before second beep
                    buzzer.next_change_ms = now_ms + 150;
                } else {
                    // Wait for next pair
                    buzzer.beep_count = 0;
                    buzzer.next_change_ms = now_ms + 650;
                }
            }
            break;
        case BeepPattern::GAME_START:
            // 1 Hz pattern for 3 seconds, then 2 seconds ON
            if (buzzer.beep_count < 3) {

                buzzer.on = !buzzer.on;
                buzzer_output(buzzer.on);

                if (buzzer.on) {
                    buzzer.next_change_ms = now_ms + 100;
                } else {
                    buzzer.beep_count++;

                    if (buzzer.beep_count < 3) {
                        buzzer.next_change_ms = now_ms + 900;
                    } else {
                        // Finished 3 beeps, turn ON solid for 2 seconds
                        buzzer.on = true;
                        buzzer_output(true);
                        buzzer.next_change_ms = now_ms + 2000;
                    }
                }

            } else {
                // Finished 2 second solid tone
                buzzer.on = false;
                buzzer_output(false);
                buzzer.pattern = BeepPattern::NONE;
            }
            break;
        case BeepPattern::GAME_OVER:
            // 3 seconds ON, then OFF
            if (!buzzer.on) {
                buzzer.on = true;
                buzzer_output(true);
                buzzer.next_change_ms = now_ms + 3000;
            }
            else {
                buzzer.on = false;
                buzzer_output(false);
                buzzer.pattern = BeepPattern::NONE;
            }

            break;
        case BeepPattern::OVERTIME:
        {
            float remaining_ratio = (float)overtime_remaining_ms / overtime_max_ms;

            remaining_ratio = std::clamp(
                remaining_ratio, 0.0f, 1.0f
            );

            float progress = 1.0f - remaining_ratio;

            // Nonlinear acceleration: 1 Hz -> 5 Hz
            float frequency = 1.0f + 4.0f * (progress * progress);

            buzzer.on = !buzzer.on;
            buzzer_output(buzzer.on);

            if (buzzer.on) {
                buzzer.next_change_ms = now_ms + 100;
            }
            else {
                uint32_t off_ms;
                uint32_t period_ms =
                    (uint32_t)(1000.0f / frequency);
                
                    if (period_ms > 100){
                        off_ms = period_ms - 100;
                    } else {
                        off_ms = 1;
                    }

                buzzer.next_change_ms = now_ms + off_ms;
            }
            break;
        }
        case BeepPattern::NONE:
            break;
    }
}

static uint32_t hsv_to_grb(uint16_t hue, uint8_t saturation, uint8_t value) {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    const uint16_t maxHue = 6 * 255;
    hue %= maxHue;
    const uint8_t section = hue / 255;
    const uint8_t offset = hue % 255;
    const uint32_t chroma = (uint32_t) value * saturation / 255;
    const uint8_t x = (uint8_t) ((uint32_t) chroma * offset / 255);
    const uint8_t m = value - chroma;

    switch (section) {
        case 0:
            red = value;
            green = x + m;
            blue = m;
            break;
        case 1:
            red = x + m;
            green = value;
            blue = m;
            break;
        case 2:
            red = m;
            green = value;
            blue = x + m;
            break;
        case 3:
            red = m;
            green = x + m;
            blue = value;
            break;
        case 4:
            red = x + m;
            green = m;
            blue = value;
            break;
        case 5:
            red = value;
            green = m;
            blue = x + m;
            break;
    }

    return urgb_u32(red, green, blue);
}

static uint32_t hsv_to_grb_scaled(uint16_t hue, float brightness_scale) {
    // Convert HSV to GRB with scaled brightness (value component).
    // This creates a simpler dim effect compared to gamma-corrected intensity scaling.
    uint8_t scaled_value = (uint8_t)(255.0f * brightness_scale + 0.5f);
    return hsv_to_grb(hue, 255, scaled_value);
}

static uint32_t scale_color_intensity(uint32_t pixel_grb, float scale) {
    const float gamma = 2.2f;
    const float inv_gamma = 1.0f / gamma;

    uint8_t old_r = (pixel_grb & 0xFF00) >> 8;
    uint8_t old_g = (pixel_grb & 0xFF0000) >> 16;
    uint8_t old_b = pixel_grb & 0xFF;

    float new_r = powf(old_r / 255.0f, gamma);
    float new_g = powf(old_g / 255.0f, gamma);
    float new_b = powf(old_b / 255.0f, gamma);

    new_r *= scale;
    new_g *= scale;
    new_b *= scale;

    new_r = powf(new_r, inv_gamma) * 255.0f + 0.5f;
    new_g = powf(new_g, inv_gamma) * 255.0f + 0.5f;
    new_b = powf(new_b, inv_gamma) * 255.0f + 0.5f;

    return urgb_u32((uint8_t) new_r, (uint8_t) new_g, (uint8_t) new_b);
}

static uint32_t blend_colors(uint32_t color_a, uint32_t color_b, float rate) {
    const float gamma = 2.2f;
    const float inv_gamma = 1.0f / gamma;

    uint8_t a_r = (color_a & 0xFF00) >> 8;
    uint8_t a_g = (color_a & 0xFF0000) >> 16;
    uint8_t a_b = color_a & 0xFF;

    uint8_t b_r = (color_b & 0xFF00) >> 8;
    uint8_t b_g = (color_b & 0xFF0000) >> 16;
    uint8_t b_b = color_b & 0xFF;

    float new_r = (1.0f - rate) * powf(a_r / 255.0f, gamma) + rate * powf(b_r / 255.0f, gamma);
    float new_g = (1.0f - rate) * powf(a_g / 255.0f, gamma) + rate * powf(b_g / 255.0f, gamma);
    float new_b = (1.0f - rate) * powf(a_b / 255.0f, gamma) + rate * powf(b_b / 255.0f, gamma);

    new_r = powf(new_r, inv_gamma) * 255.0f + 0.5f;
    new_g = powf(new_g, inv_gamma) * 255.0f + 0.5f;
    new_b = powf(new_b, inv_gamma) * 255.0f + 0.5f;

    return urgb_u32((uint8_t) new_r, (uint8_t) new_g, (uint8_t) new_b);
}


// Fills bar with the moving led pattern of bright, dim and very dim 
static void fill_bar(uint32_t bright_color, uint32_t dim_color,
    uint32_t very_dim_color, int phase) {
    for (int pixel_index = 0; pixel_index < NUM_LEDS; ++pixel_index) {
        // Shift the repeating pattern across the strip using the phase offset.
        const int pattern_position = (pixel_index + phase) % PATTERN_PERIOD;

        // Bright section: the leading portion of the moving pattern.
        if (pattern_position < PATTERN_BRIGHT_LEDS) {
            put_pixel(bright_color);
        }
        // Dim section: the middle portion of the moving pattern.
        else if (pattern_position < PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS) {
            put_pixel(dim_color);
        }
        // Very dim section: the trailing portion of the moving pattern.
        else if (pattern_position < PATTERN_BRIGHT_LEDS +
            PATTERN_DIM_LEDS + PATTERN_VERY_DIM_LEDS) {
            put_pixel(very_dim_color);
        }
    }
}

// Renders the KOTH rope as a split bar with the two teams on opposite ends.
// Blue is on the LED 0 side and Red is on the far end of the strip.
// The score fill grows outward from the middle of the rope toward each team's end.
static void render_rope(uint32_t blue_color, uint32_t red_color,
    uint32_t neutral_color, uint32_t round_timer_color, uint32_t overtime_timer_color,
    uint32_t blue_time_ms, uint32_t red_time_ms, uint32_t win_time_ms,
    bool blue_capping, bool red_capping, bool blue_won, bool red_won,
    bool round_active, bool tie_game, bool overtime_active, uint32_t now_ms,
    uint32_t round_start_ms, uint32_t round_time_ms, uint32_t overtime_end_time_ms,
    uint32_t overtime_current_max_time_ms,
    int phase_index) {
    const int center = NUM_LEDS / 2;

    float round_progress = 0.0f;
    if (round_active && round_time_ms > 0u) {
        const uint32_t round_elapsed_ms = now_ms - round_start_ms;
        if (round_elapsed_ms < round_time_ms) {
            round_progress = (float) round_elapsed_ms / (float) round_time_ms;
        } else {
            round_progress = 1.0f;
        }
    }

    const int timer_distance = (int) (round_progress * center);

    // Calculate a separate phase for the slower winning pattern
    float winning_phase = 0.0f;
    if (round_active || overtime_active) {
        winning_phase = (float)((now_ms - round_start_ms)) * PATTERN_SPEED_WINNING / 10.0f;  // Scaled to match cycle timing
        while (winning_phase >= PATTERN_PERIOD) {
            winning_phase -= PATTERN_PERIOD;
        }
    }
    const int winning_phase_index = (int) winning_phase;

    uint32_t pixel_color_bright;
    uint32_t pixel_color_dim;
    uint32_t pixel_color_very_dim;

    // Each team fills from the center of the rope out to their end.
    // Even a tiny score should light at least one LED, since the rope should always show activity.
    // Using floating-point fill allows for smooth fading at the LED boundary.
    float blue_fill = 0.0f;
    float red_fill = 0.0f;

    blue_fill = (blue_time_ms * center) / (float)win_time_ms;

    red_fill = (red_time_ms * center) / (float)win_time_ms;

    // End-of-round flash: the winning team color pulses while the strip is otherwise idle.
    const bool flash_winner = !round_active && !tie_game && (blue_won || red_won);
    bool flash_on = false;
    
    flash_on = ((now_ms / FLASH_TIME_MS) % 2u) == 0u;

    // Loop through each pixel
    for (int pixel_index = 0; pixel_index < NUM_LEDS; pixel_index++) {
        uint32_t pixel_color = neutral_color; // Start pixel off

        // Keep the center neutral unless a team has actual fill
        if (pixel_index < center) {
            const float distance_from_center = center - pixel_index;

            // Blue side fills from the middle toward LED 0.
            if (blue_fill > 0.0f && distance_from_center - 1.0f < blue_fill) {
                // Calculate fade intensity based on how much fill reaches this LED
                float intensity = blue_fill - (distance_from_center - 1.0f);
                if (intensity > 1.0f) intensity = 1.0f;  // Cap at full brightness

                // Active blue cap uses the moving pattern to make the cap feel alive.
                if (blue_capping) {
                    // Move the bright band outward from the center by subtracting the phase from
                    // the distance. As the phase increases, the band advances toward LED 0.
                    const int pattern_position = ((int)distance_from_center - phase_index + PATTERN_PERIOD) % PATTERN_PERIOD;

                    if (pattern_position < PATTERN_BRIGHT_LEDS) {
                        pixel_color = hsv_to_grb_scaled(BLUE_TEAM_HUE_CONVERTED, intensity);
                    } else if (pattern_position < PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS) {
                        pixel_color = hsv_to_grb_scaled(BLUE_TEAM_HUE_CONVERTED, 0.2f * intensity);
                    } else {
                        pixel_color = hsv_to_grb_scaled(BLUE_TEAM_HUE_CONVERTED, 0.05f * intensity);
                    }  
                } else if ((round_active || overtime_active) && blue_time_ms > red_time_ms) {
                    // Show slower winning pattern when blue is ahead but not actively capping
                    const int pattern_position = ((int)distance_from_center - winning_phase_index + PATTERN_PERIOD) % PATTERN_PERIOD;

                    if (pattern_position < PATTERN_BRIGHT_LEDS) {
                        pixel_color = hsv_to_grb_scaled(BLUE_TEAM_HUE_CONVERTED, intensity);
                    } else if (pattern_position < PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS) {
                        pixel_color = hsv_to_grb_scaled(BLUE_TEAM_HUE_CONVERTED, 0.2f * intensity);
                    } else {
                        pixel_color = hsv_to_grb_scaled(BLUE_TEAM_HUE_CONVERTED, 0.05f * intensity);
                    }
                } else {
                    // No capping and not ahead: show base color with fade
                    pixel_color = hsv_to_grb_scaled(BLUE_TEAM_HUE_CONVERTED, intensity);
                }
            }
        } else if (pixel_index >= center) {
            const float distance_from_center = pixel_index - center + 1; // Need to offset by 1 for red side since center is really 35

            // Red side fills from the middle toward the far end of the strip.
            if (red_fill > 0.0f && distance_from_center - 1.0f < red_fill) {
                // Calculate fade intensity based on how much fill reaches this LED
                float intensity = red_fill - (distance_from_center - 1.0f);
                if (intensity > 1.0f) intensity = 1.0f;  // Cap at full brightness

                // Active red cap uses the moving pattern to make the cap feel alive.
                if (red_capping) {
                    // Move the bright band outward from the center by subtracting the phase from
                    // the distance. As the phase increases, the band advances toward the far end.
                    const int pattern_position = ((int)distance_from_center - phase_index + PATTERN_PERIOD) % PATTERN_PERIOD;

                    if (pattern_position < PATTERN_BRIGHT_LEDS) {
                        pixel_color = hsv_to_grb_scaled(RED_TEAM_HUE_CONVERTED, intensity);
                    } else if (pattern_position < PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS) {
                        pixel_color = hsv_to_grb_scaled(RED_TEAM_HUE_CONVERTED, 0.2f * intensity);
                    } else {
                        pixel_color = hsv_to_grb_scaled(RED_TEAM_HUE_CONVERTED, 0.05f * intensity);
                    }  
                } else if ((round_active || overtime_active) && red_time_ms > blue_time_ms) {
                    // Show slower winning pattern when red is ahead but not actively capping
                    const int pattern_position = ((int)distance_from_center - winning_phase_index + PATTERN_PERIOD) % PATTERN_PERIOD;

                    if (pattern_position < PATTERN_BRIGHT_LEDS) {
                        pixel_color = hsv_to_grb_scaled(RED_TEAM_HUE_CONVERTED, intensity);
                    } else if (pattern_position < PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS) {
                        pixel_color = hsv_to_grb_scaled(RED_TEAM_HUE_CONVERTED, 0.2f * intensity);
                    } else {
                        pixel_color = hsv_to_grb_scaled(RED_TEAM_HUE_CONVERTED, 0.05f * intensity);
                    }
                } else {
                    // No capping and not ahead: show base color with fade
                    pixel_color = hsv_to_grb_scaled(RED_TEAM_HUE_CONVERTED, intensity);
                }
            }
        }

        // A moving flashing LED marks the remaining round time by sweeping out from the center.
        // One marker moves toward each team's end so the timer is visible on both sides.
        if (round_active) {
            const int blue_timer_led = center - timer_distance;
            const int red_timer_led = center + timer_distance;
            
            //          [35] [36]
            //         t  b | r  t 
            if ((pixel_index >= blue_timer_led - ROUND_TIME_LED_EXTRA_LENGTH &&
                pixel_index <= blue_timer_led)||(
                pixel_index >= red_timer_led - ROUND_TIME_LED_EXTRA_LENGTH &&
                pixel_index <= red_timer_led)) {
                // Flash the timer LED to make it stand out
                if (flash_on) {
                    pixel_color = round_timer_color;
                } else {
                    //pixel_color = 0u; // Turn off led completely for better visibility
                }
            }
        }
        
        // A separate overtime indicator: purple flashing LEDs during overtime
        if (overtime_active) {
            bool overtime_flash_on = false;
            if ((now_ms / OVERTIME_FLASH_TIME_MS) % 2u == 0u) {
                overtime_flash_on = true;
            }

            // Calculate progress through overtime (similar to round timer)
            uint32_t overtime_elapsed_ms = 0u;
            if (now_ms < overtime_end_time_ms) {
                overtime_elapsed_ms = overtime_end_time_ms - now_ms;
            }
            uint32_t overtime_duration_ms = OVERTIME_MS;
            if (overtime_current_max_time_ms > 0u) {
                overtime_duration_ms = overtime_current_max_time_ms;
            }
            float overtime_progress = 1.0f -
                ((float)overtime_elapsed_ms / (float)overtime_duration_ms);
            if (overtime_progress < 0.0f) overtime_progress = 0.0f;
            if (overtime_progress > 1.0f) overtime_progress = 1.0f;
            
            const int overtime_timer_distance = (int)(overtime_progress * center);
            const int blue_overtime_led = center - overtime_timer_distance;
            const int red_overtime_led = center + overtime_timer_distance;
            
            if ((pixel_index >= blue_overtime_led - ROUND_TIME_LED_EXTRA_LENGTH &&
                pixel_index <= blue_overtime_led) || 
                (pixel_index >= red_overtime_led - ROUND_TIME_LED_EXTRA_LENGTH &&
                pixel_index <= red_overtime_led)) {
                // Flash the overtime timer LED
                if (overtime_flash_on) {
                    pixel_color = overtime_timer_color;
                }
            }
        }

        // Flash the winning team color after the round ends.
        if (flash_winner && !flash_on) {
            // Probably will never happen that blue and red won at same time
            if (tie_game){ // flash both teams colors
                pixel_color = 0u;
            } else if (blue_won && pixel_index < center) { // when blue won and pixel is on blue side turn off
                pixel_color = 0u;
            } else if (red_won && pixel_index >= center) {
                pixel_color = 0u;
            }
        }

        put_pixel(pixel_color);
    }
}

int main() {
    stdio_init_all();

    gpio_init(BLUE_BUTTON_PIN);
    gpio_init(BOOT_BUTTON_PIN);
    gpio_init(RED_BUTTON_PIN);
    gpio_init(BUZZER_PIN);

    

    gpio_set_dir(BLUE_BUTTON_PIN, GPIO_IN);
    gpio_set_dir(BOOT_BUTTON_PIN, GPIO_IN);
    gpio_set_dir(RED_BUTTON_PIN, GPIO_IN);
    gpio_set_dir(BUZZER_PIN, GPIO_OUT);

    gpio_pull_up(BLUE_BUTTON_PIN);
    gpio_pull_up(BOOT_BUTTON_PIN);
    gpio_pull_up(RED_BUTTON_PIN);
    
    gpio_put(BUZZER_PIN, false);

    adc_init();
    adc_gpio_init(THROTTLE_ADC_GPIO);
    adc_select_input(THROTTLE_ADC_CHANNEL);

    // Init i2c and configure it's GPIO pins
    // i2c_init(i2c_default, 400 * 1000);   // Standard i2c clock (400kHz)
    i2c_init(i2c_default, 1000 * 1000);     // Fast clock (1000 kHz), some SSD1306 devices may work up to 1100-1200 kHz
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_pull_up(PICO_DEFAULT_I2C_SCL_PIN);

    // Init display with SSD1309 driver IC and a reset pin
    pico_oled display(OLED_SSD1309, DISPLAY_I2C_ADDR, DISPLAY_WIDTH, DISPLAY_HEIGHT, /*reset_gpio=*/ 15);   

    // Init display with SSD1306 driver IC
    // pico_oled display(OLED_SSD1306, DISPLAY_I2C_ADDR, DISPLAY_WIDTH, DISPLAY_HEIGHT);       

    display.oled_init();
    display.set_font(press_start_2p);
    display.fill(0);    // Clear display    
    display.set_cursor(0,0);

    const uint offset = pio_add_program(pio0, &ws2812_program);
    ws2812_program_init(pio0, 0, offset, WS2812_PIN, 800000, false);

    const uint32_t blue_team_color = hsv_to_grb(BLUE_TEAM_HUE_CONVERTED, BLUE_TEAM_HUE_SATURATION, 255);   // Blue team color.
    const uint32_t red_team_color = hsv_to_grb(RED_TEAM_HUE_CONVERTED, RED_TEAM_HUE_SATURATION, 255);      // Red team color.
    const uint32_t round_timer_color = hsv_to_grb(ROUND_TIMER_HUE_CONVERTED, ROUND_TIMER_HUE_SATURATION, 200); // Round timer LED.
    const uint32_t overtime_timer_color = hsv_to_grb(OVERTIME_TIMER_HUE_CONVERTED, OVERTIME_TIMER_HUE_SATURATION, 200); // Overtime timer LED.
    const uint32_t neutral_rope_color = 0u;                        // Dark rope when idle.

    const uint32_t win_time_ms = DEFAULT_WIN_TIME * 1000u;
    const uint32_t round_time_ms = DEFAULT_ROUND_TIME * 1000u;
    const uint32_t round_start_ms = to_ms_since_boot(get_absolute_time());


    uint32_t blue_time_ms = 0;
    uint32_t red_time_ms = 0;
    uint32_t last_update_ms = round_start_ms;
    bool round_active = true;
    bool blue_won = false;
    bool red_won = false;
    bool blue_leading = false;
    bool red_leading = false;
    bool tie_game = false;
    bool game_over = false;
    bool game_over_play_sound = true;
    float phase = 0.0f;
    
    // Overtime state
    uint32_t overtime_current_max_time_ms = 0;
    uint32_t overtime_time_remaining_ms = 0;
    uint32_t overtime_end_time_ms = 0;
    bool overtime_active = false;
    uint8_t overtime_flips = 0;  // Track how many times lead changed during overtime

    while (true) {
        /*
        Classic Koth Game Logic
        2 teams, Red and Blue.
        Each team accumulates time when their team's button is pressed
        Team wins when their time reaches win time.
        Round is over when round time has elapsed. Team with more time wins.
        Overtime mechanic: 5 second for losing team to start capping. Resets everytime they let go of button.

        LED rope logic.
        The rope is split into 2 halves. 
        The RED team is the far end of the strip (aka the top)
        The BLUE team is the LED 0 end of the strip (aka the bottom)
        When 1 team is accumulating points, their time gained is map from the center of the led bar to their team's end. They moving led pattern also is playing towards their end goal when they are capping.
        At end of round, the winning team colours will flash.
        */
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        overtime_time_remaining_ms = overtime_end_time_ms - now_ms;
        update_buzzer(now_ms, overtime_time_remaining_ms);
        const uint32_t dt_ms = now_ms - last_update_ms;
        last_update_ms = now_ms;

        const bool blue_pressed = button_pressed(BLUE_BUTTON_PIN);
        const bool red_pressed = button_pressed(RED_BUTTON_PIN);

        display.fill(0);    // Clear display    
        display.set_cursor(0,0);

        if (round_active) {
            if (blue_pressed && !red_pressed) {
                blue_time_ms += dt_ms;
            } else if (red_pressed && !blue_pressed) {
                red_time_ms += dt_ms;
            }
            
            // check who leading
            if (blue_time_ms > red_time_ms) {
                blue_leading = true;
                red_leading = false;
            } else if (red_time_ms > blue_time_ms) {
                red_leading = true;
                blue_leading = false;
            } else {
                blue_leading = false;
                red_leading = false;
            }
            const uint32_t round_elapsed_ms = now_ms - round_start_ms;

            // if either team score reaches win time
            if (blue_time_ms >= win_time_ms || red_time_ms >= win_time_ms) {
                round_active = false;
                game_over = true;
                blue_won = blue_time_ms > red_time_ms;
                red_won = red_time_ms > blue_time_ms;
                // Round Time up
            } else if (round_elapsed_ms >= round_time_ms) {
                // Start overtime when round time is up
                overtime_active = true;
                overtime_flips = 0;
                overtime_current_max_time_ms = OVERTIME_MS;
                overtime_end_time_ms = now_ms + OVERTIME_MS;
                round_active = false;
                blue_won = false;
                red_won = false;
            }
        } else if (overtime_active) {
            // Overtime mechanic: losing team gets time to cap
            // Each time they release and try again, timer resets
            // If they take the lead, the other team gets overtime at reduced duration


            // Check if lead flipped during overtime
            bool lead_flipped = false;
            if (blue_leading && red_time_ms > blue_time_ms) {
                blue_leading = false;
                red_leading = true;
                lead_flipped = true;
                overtime_flips++;
            } else if (red_leading && blue_time_ms > red_time_ms) {
                red_leading = false;
                blue_leading = true;
                lead_flipped = true;
                overtime_flips++;
            }
            
            // If lead flipped, give the new winning team reduced time by half for each flip
            if (lead_flipped) {
                float time_multiplier = powf(0.5f, (float)overtime_flips);
                overtime_current_max_time_ms = (uint32_t)(OVERTIME_MS * time_multiplier);
                overtime_end_time_ms = now_ms + overtime_current_max_time_ms;
            }
            
            // Keep overtime full while the non-leading team is capping.
            bool blue_capping_now = !blue_leading && blue_pressed && !red_pressed;
            bool red_capping_now = !red_leading && red_pressed && !blue_pressed;
            
            // Reset overtime timer if the losing team is actively capping
            if (blue_capping_now || red_capping_now) {
                overtime_end_time_ms = now_ms + overtime_current_max_time_ms;
            }
            
            // Acculate time for the team that is currently capping during overtime
            if (red_pressed && !blue_pressed) {
                red_time_ms += dt_ms;
            } else if (blue_pressed && !red_pressed) {
                blue_time_ms += dt_ms;
            }
            
            // Check if overtime expired
            if (now_ms >= overtime_end_time_ms) {
                overtime_active = false;
                // Overtime expired - game is truly over
                game_over = true;
                // Determine team winner
                if (blue_time_ms > red_time_ms) {
                    blue_won = true;
                } else if (red_time_ms > blue_time_ms) {
                    red_won = true;
                } else {
                    // This basically should never happen
                    tie_game = true;
                }
            }
        }

        // Read the throttle ADC and use it to control the moving pattern speed.
        // This lets the rope animate faster or slower based on the live input signal.
        uint16_t raw = adc_read();
        float throttle_normalized = raw / 4095.0f;
        float pattern_speed = PATTERN_SPEED;

        
        // Map the ADC value to a usable speed range. Higher throttle means faster motion.
        // float pattern_speed = PATTERN_SPEED + (throttle_normalized * (PATTERN_SPEED_MAX - PATTERN_SPEED));
        // if (pattern_speed < PATTERN_SPEED_MIN) {
        //     pattern_speed = PATTERN_SPEED_MIN;
        // }
        // if (pattern_speed > PATTERN_SPEED_MAX) {
        //     pattern_speed = PATTERN_SPEED_MAX;
        // }

        // Advance the pattern by a fractional step so the rope can speed up or slow down.
        // This also keeps the pattern moving smoothly instead of stalling when the speed is < 1.
        phase += PATTERN_SPEED;
        if (phase >= PATTERN_PERIOD) {
            phase -= PATTERN_PERIOD;
        }

        // The rope renders the score state instead of the ADC test pattern.
        bool blue_capping = false;
        bool red_capping = false;
        if (round_active) {
            blue_capping = blue_pressed && !red_pressed;
            red_capping = red_pressed && !blue_pressed;
        } else if (overtime_active) {
            blue_capping = blue_pressed && !red_pressed;
            red_capping = red_pressed && !blue_pressed;
        }

        // This render uses a centered fill: the score grows from the middle toward each side.
        const int phase_index = (int) phase;
        render_rope(blue_team_color,
                    red_team_color,
                    neutral_rope_color,
                    round_timer_color,
                    overtime_timer_color,
                    blue_time_ms,
                    red_time_ms,
                    win_time_ms,
                    blue_capping,
                    red_capping,
                    blue_won,
                    red_won,
                    round_active,
                    tie_game,
                    overtime_active,
                    now_ms,
                    round_start_ms,
                    round_time_ms,
                    overtime_end_time_ms,
                    overtime_current_max_time_ms,
                    phase_index);
        

        // Play sounds based on gameplay state
        // Testing Round active = 1 hz beep
        if(blue_capping || red_capping){
            start_beep_pattern(BeepPattern::DOUBLE_ONE_HZ,now_ms);
        }
        else if (overtime_active){
            start_beep_pattern(BeepPattern::OVERTIME,now_ms);
        } else if (game_over){ // Games over
            if (game_over_play_sound)
                play_beep_pattern_once(BeepPattern::GAME_OVER,now_ms);
                game_over_play_sound = false; // play sound once
        } else {
            stop_buzzer();
        }
        
        
        const char *round_status = "done";
        if (round_active) {
            round_status = "live";
        } else if (tie_game) {
            round_status = "tie";
        }

        printf("B:%lu R:%lu blue:%d red:%d boot:%d round:%s pattern_speed:%f\r",
               (unsigned long) blue_time_ms,
               (unsigned long) red_time_ms,
               blue_pressed,
               red_pressed,
               button_pressed(BOOT_BUTTON_PIN),
               round_status,
                pattern_speed);
        // also show debug on display
        display.print_num("B:%d\n",blue_time_ms);
        display.print_num("R:%d\n",red_time_ms);
        if (round_active){
            display.print("Round: 1\n");
        } else {
            display.print("Round: 0\n");
        }
        if (overtime_active){
            display.print("Overtime: 1\n");
        } else {
            display.print("Overtime: 0\n");
        }
        display.print_num("Overtimeflips: %d\n",overtime_flips);
        if (buzzer.on){
            display.print("Buzzer: 1\n");
        } else {
            display.print("Buzzer: 0\n");
        }
        
        // Do button stuff here
        if (button_pressed(BOOT_BUTTON_PIN)) {
            sleep_ms(20);
            if (button_pressed(BOOT_BUTTON_PIN)) {
                reset_usb_boot(0, 0);
            }
        }
        
        display.render();
        sleep_ms(10); // Need sleep to show leds
    }
    return 0;
}