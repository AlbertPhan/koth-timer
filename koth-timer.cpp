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

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

extern "C" {
#include "pico/bootrom.h"
}
// Pins
#define WS2812_PIN 3
#define BLUE_BUTTON_PIN 2
#define BOOT_BUTTON_PIN 16
#define RED_BUTTON_PIN 6
#define THROTTLE_ADC_GPIO 26
#define THROTTLE_ADC_CHANNEL 0
#define NUM_LEDS 70 // 150 is the led rope but it's too big
#define ADC_REFERENCE_VOLTAGE 3.3f

// Game Stuff
#define DEFAULT_WIN_TIME 60 // Time to hold to win round
#define DEFAULT_ROUND_TIME 300 // Time for round to end

#define FLASH_TIME_MS 500 // Time between flashing

// LED 
#define PATTERN_BRIGHT_LEDS 6
#define PATTERN_DIM_LEDS 4
#define PATTERN_VERY_DIM_LEDS 2
#define PATTERN_PERIOD (PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS + PATTERN_VERY_DIM_LEDS)
#define PATTERN_SPEED 0.9f // 0.9 will be good
#define PATTERN_SPEED_MIN 0.2f
#define PATTERN_SPEED_MAX 1.5f // After testing the speed remove min and max "throttle" usage and set to pattern speed
#define ROUND_TIME_LED_EXTRA_LENGTH 1 // amount of extra leds (half) from

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
#define RED_TEAM_HUE_CONVERTED (RED_TEAM_HUE * HUE_FACTOR)
#define BLUE_TEAM_HUE_CONVERTED (BLUE_TEAM_HUE * HUE_FACTOR)
#define ROUND_TIMER_HUE_CONVERTED (ROUND_TIMER_HUE * HUE_FACTOR)// Purple (approx 270 degrees)

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t) r << 8) | ((uint32_t) g << 16) | b;
}

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);
}

static inline bool button_pressed(uint gpio) {
    return !gpio_get(gpio);
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
    uint32_t neutral_color, uint32_t round_timer_color,
    uint32_t blue_time_ms, uint32_t red_time_ms, uint32_t win_time_ms,
    bool blue_capping, bool red_capping, bool blue_won, bool red_won,
    bool round_active, bool tie_game, uint32_t now_ms,
    uint32_t round_start_ms, uint32_t round_time_ms, int phase_index) {
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

    // Each team fills from the center of the rope out to their end.
    // Even a tiny score should light at least one LED, since the rope should always show activity.
    int blue_fill = 0;
    int red_fill = 0;

    blue_fill = (int) ((blue_time_ms * center) / win_time_ms);
    if (blue_time_ms > 0 && blue_fill == 0) {
        blue_fill = 1;
    }

    red_fill = (int) ((red_time_ms * center) / win_time_ms);
    if (red_time_ms > 0 && red_fill == 0) {
        red_fill = 1;
    }

    // End-of-round flash: the winning team color pulses while the strip is otherwise idle.
    const bool flash_winner = !round_active && !tie_game && (blue_won || red_won);
    bool flash_on = false;
    
    flash_on = ((now_ms / FLASH_TIME_MS) % 2u) == 0u;

    // Loop through each pixel
    for (int pixel_index = 0; pixel_index < NUM_LEDS; pixel_index++) {
        uint32_t pixel_color = neutral_color; // Start pixel off

        // Keep the center neutral unless a team has actual fill
        if (pixel_index < center) {
            const int distance_from_center = center - pixel_index;

            // Blue side fills from the middle toward LED 0.
            if (blue_fill > 0 && distance_from_center <= blue_fill) {
                pixel_color = blue_color;

                // Active blue cap uses the moving pattern to make the cap feel alive.
                if (blue_capping) {
                    // Move the bright band outward from the center by subtracting the phase from
                    // the distance. As the phase increases, the band advances toward LED 0.
                    const int pattern_position = (distance_from_center - phase_index + PATTERN_PERIOD) % PATTERN_PERIOD;
                    if (pattern_position < PATTERN_BRIGHT_LEDS) {
                        pixel_color = blue_color;
                    } else if (pattern_position < PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS) {
                        pixel_color = scale_color_intensity(blue_color, 0.2f);
                    } else {
                        pixel_color = scale_color_intensity(blue_color, 0.05f);
                    }
                }
            }
        } else if (pixel_index >= center) {
            const int distance_from_center = pixel_index - center + 1; // Need to offset by 1 for red side since center is really 35

            // Red side fills from the middle toward the far end of the strip.
            if (red_fill > 0 && distance_from_center <= red_fill) {
                pixel_color = red_color;

                // Active red cap uses the moving pattern to make the cap feel alive.
                if (red_capping) {
                    // Move the bright band outward from the center by subtracting the phase from
                    // the distance. As the phase increases, the band advances toward the far end.
                    const int pattern_position = (distance_from_center - phase_index + PATTERN_PERIOD) % PATTERN_PERIOD;
                    if (pattern_position < PATTERN_BRIGHT_LEDS) {
                        pixel_color = red_color;
                    } else if (pattern_position < PATTERN_BRIGHT_LEDS + PATTERN_DIM_LEDS) {
                        pixel_color = scale_color_intensity(red_color, 0.2f);
                    } else {
                        pixel_color = scale_color_intensity(red_color, 0.05f);
                    }
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

        // Flash the winning team color after the round ends.
        if (flash_winner && !flash_on) {
            if (blue_won && pixel_index < center) { // when blue won and pixel is on blue side turn off
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

    gpio_set_dir(BLUE_BUTTON_PIN, GPIO_IN);
    gpio_set_dir(BOOT_BUTTON_PIN, GPIO_IN);
    gpio_set_dir(RED_BUTTON_PIN, GPIO_IN);

    gpio_pull_up(BLUE_BUTTON_PIN);
    gpio_pull_up(BOOT_BUTTON_PIN);
    gpio_pull_up(RED_BUTTON_PIN);

    adc_init();
    adc_gpio_init(THROTTLE_ADC_GPIO);
    adc_select_input(THROTTLE_ADC_CHANNEL);

    const uint offset = pio_add_program(pio0, &ws2812_program);
    ws2812_program_init(pio0, 0, offset, WS2812_PIN, 800000, false);

    const uint32_t blue_team_color = hsv_to_grb(BLUE_TEAM_HUE_CONVERTED, BLUE_TEAM_HUE_SATURATION, 255);   // Blue team color.
    const uint32_t red_team_color = hsv_to_grb(RED_TEAM_HUE_CONVERTED, RED_TEAM_HUE_SATURATION, 255);      // Red team color.
    const uint32_t round_timer_color = hsv_to_grb(ROUND_TIMER_HUE_CONVERTED, ROUND_TIMER_HUE_SATURATION, 200); // Round timer LED.
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
    bool tie_game = false;
    float phase = 0.0f;

    while (true) {
        /*
        Classic Koth Game Logic
        2 teams, Red and Blue.
        Each team accumulates time when their team's button is pressed
        Team wins when their time reaches win time.
        Round is over when round time has elapsed. Team with more time wins.
        Overtime mechanic: If losing team is pressing button when game ends, they can accumulate time until button is let go.

        LED rope logic.
        The rope is split into 2 halves. 
        The RED team is the far end of the strip (aka the top)
        The BLUE team is the LED 0 end of the strip (aka the bottom)
        When 1 team is accumulating points, their time gained is map from the center of the led bar to their team's end. They moving led pattern also is playing towards their end goal when they are capping.
        At end of round, the winning team colours will flash.
        */
        const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        const uint32_t dt_ms = now_ms - last_update_ms;
        last_update_ms = now_ms;

        const bool blue_pressed = button_pressed(BLUE_BUTTON_PIN);
        const bool red_pressed = button_pressed(RED_BUTTON_PIN);

        if (round_active) {
            if (blue_pressed && !red_pressed) {
                blue_time_ms += dt_ms;
            } else if (red_pressed && !blue_pressed) {
                red_time_ms += dt_ms;
            }

            const uint32_t round_elapsed_ms = now_ms - round_start_ms;

            if (blue_time_ms >= win_time_ms || red_time_ms >= win_time_ms) {
                round_active = false;
                blue_won = blue_time_ms > red_time_ms;
                red_won = red_time_ms > blue_time_ms;
                if (blue_won) {
                    red_won = false;
                }
                if (red_won) {
                    blue_won = false;
                }
            } else if (round_elapsed_ms >= round_time_ms) {
                round_active = false;
                blue_won = false;
                red_won = false;
                if (blue_time_ms > red_time_ms) {
                    blue_won = true;
                } else if (red_time_ms > blue_time_ms) {
                    red_won = true;
                } else {
                    // This basically should never happen
                    tie_game = true;
                }
            }
        } else if (!tie_game) {
            // Overtime: only the losing team can continue scoring while it is still holding the button.
            // TODO: make it when they let go of button overtime is over.
            if (blue_won && red_pressed && !blue_pressed) {
                red_time_ms += dt_ms;
            } else if (red_won && blue_pressed && !red_pressed) {
                blue_time_ms += dt_ms;
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
        const bool blue_capping = round_active && blue_pressed && !red_pressed;
        const bool red_capping = round_active && red_pressed && !blue_pressed;

        // This render uses a centered fill: the score grows from the middle toward each side.
        const int phase_index = (int) phase;
        render_rope(blue_team_color,
                    red_team_color,
                    neutral_rope_color,
                    round_timer_color,
                    blue_time_ms,
                    red_time_ms,
                    win_time_ms,
                    blue_capping,
                    red_capping,
                    blue_won,
                    red_won,
                    round_active,
                    tie_game,
                    now_ms,
                    round_start_ms,
                    round_time_ms,
                    phase_index);

        printf("B:%lu R:%lu blue:%d red:%d boot:%d round:%s pattern_speed:%f\r",
               (unsigned long) blue_time_ms,
               (unsigned long) red_time_ms,
               blue_pressed,
               red_pressed,
               button_pressed(BOOT_BUTTON_PIN),
               round_active ? "live" : (tie_game ? "tie" : "done"),
                pattern_speed);

        // Do button stuff here
        if (button_pressed(BOOT_BUTTON_PIN)) {
            sleep_ms(20);
            if (button_pressed(BOOT_BUTTON_PIN)) {
                reset_usb_boot(0, 0);
            }
        }

        sleep_ms(10); // Need sleep to show leds
    }
    return 0;
}