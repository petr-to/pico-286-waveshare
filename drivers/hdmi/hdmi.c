#pragma GCC optimize("O3")
#include <stdalign.h>
#include <hardware/dma.h>
#include <hardware/pio.h>
#include <hardware/clocks.h>

#include "emulator/emulator.h"
#include "graphics.h"

// PIO parameters
static uint pio_program_offset_video = 0;
static uint pio_program_offset_converter = 0;

// State machines
static int sm_video_output = -1;
static int sm_address_converter = -1;

// Active video mode
static enum graphics_mode_t graphics_mode = TEXTMODE_80x25_COLOR;

// Graphics framebuffer
static uint8_t *graphics_framebuffer = NULL;
static int framebuffer_width = 0;
static int framebuffer_height = 0;
static int framebuffer_offset_x = 0;
static int framebuffer_offset_y = 0;

// Text buffer
uint8_t *text_buffer = NULL;

// DMA channels for primary graphics buffer
static int dma_channel_control;
static int dma_channel_data;

// DMA channels for palette conversion
static int dma_channel_palette_control;
static int dma_channel_palette_data;

// Scanline buffers
static uint32_t *scanline_buffers[2] = { NULL,NULL };
static uint32_t *dma_buffer_addresses[2];

// DMA palette buffer for conversion (dma_data allocated at the end)
static alignas(4096) uint32_t tmds_palette_buffer[2256];

// Interrupt counter for hang detection
static uint32_t interrupt_counter = 0;

// HDMI control constants
#define HDMI_CTRL_BASE_INDEX (251)

extern int cursor_blink_state;

/**
 * PIO program for address conversion in palette lookup
 * This program converts 8-bit palette indices to TMDS-encoded RGB data
 */
uint16_t pio_instructions_address_converter[] = {
    0x80a0, //  0: pull   block           ; Get palette index from DMA
    0x40e8, //  1: in     osr, 8          ; Shift 8 bits into ISR
    0x4034, //  2: in     x, 20           ; Shift 20 bits from X (base address)
    0x8020, //  3: push   block           ; Push converted address to output
};


const struct pio_program pio_program_address_converter = {
    .instructions = pio_instructions_address_converter,
    .length = 4,
    .origin = -1,
};

/**
 * PIO program for HDMI video output
 * Outputs 6 bits per clock cycle with proper side-set clock generation
 */
const uint16_t pio_instructions_hdmi_output[] = {
    0x7006, //  0: out    pins, 6         side 2  ; Output 6 data bits, clock high
    0x7006, //  1: out    pins, 6         side 2  ; Output 6 data bits, clock high
    0x7006, //  2: out    pins, 6         side 2  ; Output 6 data bits, clock high
    0x7006, //  3: out    pins, 6         side 2  ; Output 6 data bits, clock high
    0x7006, //  4: out    pins, 6         side 2  ; Output 6 data bits, clock high
    0x6806, //  5: out    pins, 6         side 1  ; Output 6 data bits, clock low
    0x6806, //  6: out    pins, 6         side 1  ; Output 6 data bits, clock low
    0x6806, //  7: out    pins, 6         side 1  ; Output 6 data bits, clock low
    0x6806, //  8: out    pins, 6         side 1  ; Output 6 data bits, clock low
    0x6806, //  9: out    pins, 6         side 1  ; Output 6 data bits, clock low
};

static const struct pio_program pio_program_hdmi_output = {
    .instructions = pio_instructions_hdmi_output,
    .length = 10,
    .origin = -1,
};

/**
 * Generate TMDS differential pair data for RGB channels
 * @param red_data 10-bit TMDS encoded red channel data
 * @param green_data 10-bit TMDS encoded green channel data
 * @param blue_data 10-bit TMDS encoded blue channel data
 * @return 64-bit serialized differential pair data
 */
static uint64_t generate_hdmi_differential_data(const uint16_t red_data,
                                                const uint16_t green_data,
                                                const uint16_t blue_data) {
    uint64_t serialized_output = 0;

    // Process each of the 10 bits in the TMDS data
    for (int bit_index = 0; bit_index < 10; bit_index++) {
        serialized_output <<= 6;
        if (bit_index == 5) serialized_output <<= 2; // Extra shift for timing

        // Extract current bit from each channel
        uint8_t red_bit = (red_data >> (9 - bit_index)) & 1;
        uint8_t green_bit = (green_data >> (9 - bit_index)) & 1;
        uint8_t blue_bit = (blue_data >> (9 - bit_index)) & 1;

        // Create differential pairs (bit and its inverse)
        red_bit |= (red_bit ^ 1) << 1;
        green_bit |= (green_bit ^ 1) << 1;
        blue_bit |= (blue_bit ^ 1) << 1;

#if HDMI_PIN_invert_diffpairs
        // Apply differential pair inversion if configured
        red_bit ^= 0b11;
        green_bit ^= 0b11;
        blue_bit ^= 0b11;
#endif

        // Pack into a 6-bit output word
#if HDMI_PIN_RGB_notBGR
        serialized_output |= (red_bit << 4) | (green_bit << 2) | (blue_bit << 0);
#else
        serialized_output |= (blue_bit << 4) | (green_bit << 2) | (red_bit << 0);
#endif
    }
    return serialized_output;
}

/**
 * TMDS 8b/10b encoder for a single color channel
 * Implements the TMDS encoding algorithm to convert 8-bit color to 10-bit TMDS
 * @param input_byte 8-bit input color value
 * @return 10-bit TMDS encoded value
 */
static uint tmds_encode_8b10b(const uint8_t input_byte) {
    // Count number of 1s in input byte using builtin
    const int ones_count = __builtin_popcount(input_byte);

    // Determine encoding method: XOR or XNOR
    const bool use_xnor = ones_count > 4 || ones_count == 4 && (input_byte & 1) == 0;

    // Generate 8-bit encoded data
    uint16_t encoded_data = input_byte & 1; // Start with LSB
    uint16_t previous_bit = encoded_data;

    for (int i = 1; i < 8; i++) {
        const uint16_t current_bit = (input_byte >> i) & 1;
        const uint16_t encoded_bit = use_xnor ? !(previous_bit ^ current_bit) : (previous_bit ^ current_bit);
        encoded_data |= encoded_bit << i;
        previous_bit = encoded_bit;
    }

    // Stage 2: DC Balancing (Simplified for "Symbol + Inverse" transmission)
    // We assume disparity is 0.
    // If q_m[8] == 0 (use_xnor):
    //   q_out[9] = 1, q_out[8] = 0, q_out[0-7] = ~q_m[0-7]
    // If q_m[8] == 1 (!use_xnor):
    //   q_out[9] = 0, q_out[8] = 1, q_out[0-7] = q_m[0-7]

    if (use_xnor) {
        encoded_data ^= 0xFF; // Invert data bits
        encoded_data |= (1 << 9); // Set Bit 9
    } else {
        encoded_data |= (1 << 8); // Set Bit 8
    }

    return encoded_data;
}

/**
 * Set PIO state machine X register to 32-bit value
 * Used to set base address for palette lookup
 */
static inline void pio_set_x_register(PIO pio, int sm, uint32_t value) {
    pio_sm_exec(pio, sm, pio_encode_set(pio_x, 0));    // Clear X
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_null));
    pio_sm_put_blocking(pio, sm, value);
    pio_sm_exec(pio, sm, pio_encode_pull(false, false));
    pio_sm_exec(pio, sm, pio_encode_mov(pio_x, pio_osr));
}

// Spread 8 bits of a byte into positions 0,4,8,...28
static inline uint32_t spread8(uint32_t plane) {
    plane = (plane | (plane << 12)) & 0x000F000Fu;
    plane = (plane | (plane <<  6)) & 0x03030303u;
    plane = (plane | (plane <<  3)) & 0x11111111u;
    return plane;
}

// Merge 4 plane bytes [P3|P2|P1|P0] into 8 nibbles (pixel color indices).
static inline uint32_t ega_pack8_from_planes(const uint32_t ega_planes) {
    const uint32_t pixel1 = spread8(ega_planes        & 0xFFu);
    const uint32_t pixel2 = spread8((ega_planes >> 8) & 0xFFu);
    const uint32_t pixel3 = spread8((ega_planes >>16) & 0xFFu);
    const uint32_t pixel4 = spread8(ega_planes >>24);

    return pixel1 | pixel2 << 1 | pixel3 << 2 | pixel4 << 3;
}

static void __time_critical_func() hdmi_scanline_interrupt_handler() {
    static uint8_t buffer_index = 0;
    static uint16_t current_scanline = 0;

    interrupt_counter++;

    // Acknowledge DMA interrupt
    dma_hw->ints0 = 1u << dma_channel_control;

    current_scanline = current_scanline >= 524 ? 0 : (current_scanline + 1);

    const bool odd_scanline = current_scanline & 1;
    
    // VSync timing: 640x480@60Hz = lines 490-491 (480 active + 10 front porch)
    port3DA = (current_scanline >= 490 ? 8 : 0) | odd_scanline; 

    const bool is_640x400_mode = (graphics_mode == TEXTMODE_80x25_COLOR ||
                                  graphics_mode == TEXTMODE_80x25_BW);

    // Line doubling logic
    // We are preparing for the NEXT scanline (current_scanline + 1)
    // If is_640x400_mode: Always generate new data
    // If 320x200 mode:
    //   If current is Odd (1), Next is Even (2). Even lines are NEW. -> Generate.
    //   If current is Even (2), Next is Odd (3). Odd lines are REPEAT. -> Reuse.
    
    bool generate_new_line = false;
    if (is_640x400_mode) {
        generate_new_line = true;
    } else {
        if (odd_scanline) {
            generate_new_line = false; // Odd line -> Repeat
        } else {
            generate_new_line = true;  // Even line -> New
        }
    }

    if (!generate_new_line) {
        // Reuse current buffer for next scanline
        dma_channel_set_read_addr(dma_channel_control, &dma_buffer_addresses[buffer_index], false);
        return;
    }

    // Switch buffer for new data
    buffer_index ^= 1;
    dma_channel_set_read_addr(dma_channel_control, &dma_buffer_addresses[buffer_index], false);

    uint8_t *current_scanline_buffer = (uint8_t *)scanline_buffers[buffer_index];

    if (graphics_framebuffer && current_scanline < 480) {

        // Buffer offset: Front (16) + Sync (96) + Back (48) = 160
        const uint16_t buffer_offset = 160; // Same for all modes to maintain VGA timing
        uint8_t *output_buffer = current_scanline_buffer + buffer_offset;
    
        // Text mode 80x25: 400 lines (25*16) centered on 480, offset 40 lines, no line doubling
        // Graphics modes 320x200: 400 lines (200*2 line doubling) centered on 480, offset 40 lines
        const bool in_text_area = (current_scanline >= 40 && current_scanline < 440);
        const uint16_t y_raw = in_text_area ? current_scanline - 40 : 0;
        // For text mode use y_raw directly (0-399), for graphics mode y_raw/2 (0-199 with line doubling)
        const uint16_t y = is_640x400_mode ? y_raw : (y_raw / 2);

        switch (graphics_mode) {
            
            case TEXTMODE_80x25_BW: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                const uint8_t y_div_16 = y >> 4;           // 0-24 for 25 rows
                const uint8_t glyph_line = y & 15;         // 0-15 within glyph
                
                const uint32_t *text_buffer_line = &VIDEORAM[0x8000 + (vram_offset << 1) + 
                                                            __fast_mul(y_div_16, 160)];
                
                for (unsigned int column = 0; column < TEXTMODE_COLS; column++) {
                    uint8_t glyph_pixels = font_8x16[__fast_mul(*text_buffer_line++ & 0xFF, 16) + glyph_line];
                    const uint8_t color = *text_buffer_line++;
                    
                    // Blink handling
                    if (color & 0x80 && cursor_blink_state) {
                        glyph_pixels = 0;
                    }
                    
                    // Cursor rendering
                    const uint8_t cursor_active = cursor_blink_state &&
                        y_div_16 == CURSOR_Y && column == CURSOR_X &&
                        glyph_line >= 14;  // Cursor on bottom 2 lines
                    
                    if (cursor_active) {
                        const uint8_t cursor_color = textmode_palette[color & 0xf];
                        #pragma GCC unroll(8)
                        for (int i = 0; i < 8; i++) {
                            *output_buffer++ = cursor_color;
                        }
                    } else if (cga_blinking && (color & 0x80)) {
                        const uint8_t fg = textmode_palette[color & 0xf];
                        const uint8_t bg = textmode_palette[color >> 4 & 0x7];
                        #pragma GCC unroll(8)
                        for (int bit = 8; bit--;) {
                            *output_buffer++ = cursor_blink_state ? bg 
                                : (glyph_pixels & 1) ? fg : bg;
                            glyph_pixels >>= 1;
                        }
                    } else {
                        const uint8_t fg = textmode_palette[color & 0xf];
                        const uint8_t bg = textmode_palette[color >> 4];
                        #pragma GCC unroll(8)
                        for (int bit = 8; bit--;) {
                            *output_buffer++ = (glyph_pixels & 1) ? fg : bg;
                            glyph_pixels >>= 1;
                        }
                    }
                }
                break;
            }   
            case TEXTMODE_80x25_COLOR: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                const uint8_t y_div_16 = y >> 4;         // 0-24 text rows (25 rows total)
                const uint8_t glyph_line = y & 15;       // 0-15 font rows

                const uint32_t *text_buffer_line = &VIDEORAM[0x8000 + (vram_offset << 1) + 
                                                                __fast_mul(y_div_16, 160)];

                for (unsigned int column = 0; column < TEXTMODE_COLS; column++) {
                    uint8_t glyph_pixels = font_8x16[__fast_mul(*text_buffer_line++ & 0xFF, 16) + glyph_line];
                    const uint8_t color = *text_buffer_line++;
                    
                    if (color & 0x80 && cursor_blink_state) {
                        glyph_pixels = 0;
                    }
                    
                    const uint8_t cursor_active = cursor_blink_state &&
                        y_div_16 == CURSOR_Y && column == CURSOR_X &&
                        glyph_line >= 14;  // Cursor on bottom 2 rows (14-15)
                    
                    if (cursor_active) {
                        const uint8_t cursor_color = textmode_palette[color & 0xf];
                        #pragma GCC unroll(8)
                        for (int i = 0; i < 8; i++) {
                            *output_buffer++ = cursor_color;
                        }
                    } else if (cga_blinking && (color & 0x80)) {
                        const uint8_t fg = textmode_palette[color & 0xf];
                        const uint8_t bg = textmode_palette[color >> 4 & 0x7];
                        #pragma GCC unroll(8)
                        for (int bit = 8; bit--;) {
                            *output_buffer++ = cursor_blink_state ? bg
                                : (glyph_pixels & 1) ? fg : bg;
                            glyph_pixels >>= 1;
                        }
                    } else {
                        const uint8_t fg = textmode_palette[color & 0xf];
                        const uint8_t bg = textmode_palette[color >> 4];
                        #pragma GCC unroll(8)
                        for (int bit = 8; bit--;) {
                            *output_buffer++ = (glyph_pixels & 1) ? fg : bg;
                            glyph_pixels >>= 1;
                        }
                    }
                }
             break;
            }
            case CGA_320x200x4:
            case CGA_320x200x4_BW: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                const register uint32_t *cga_row = &VIDEORAM[0x8000 + (vram_offset << 1) + __fast_mul(y >> 1, 80) + ((y & 1) << 13)];
                //2bit buf with pixel doubling for VGA timing
                for (int x = 320 / 4; x--;) {
                    const uint8_t cga_byte = *cga_row++;

                    uint8_t color = cga_byte >> 6;
                    *output_buffer++ = color;
                    *output_buffer++ = color;  // Pixel doubling
                    color = (cga_byte >> 4) & 3;
                    *output_buffer++ = color;
                    *output_buffer++ = color;  // Pixel doubling
                    color = (cga_byte >> 2) & 3;
                    *output_buffer++ = color;
                    *output_buffer++ = color;  // Pixel doubling
                    color = (cga_byte >> 0) & 3;
                    *output_buffer++ = color;
                    *output_buffer++ = color;  // Pixel doubling
                }
                break;
            }
            case COMPOSITE_160x200x16_force:
            case COMPOSITE_160x200x16:
            case TGA_160x200x16: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                const register uint32_t *tga_row = &VIDEORAM[tga_offset + __fast_mul(y >> 1, 80) + ((y & 1) << 13)];
                uint32_t *output_buffer32 = (uint32_t *)output_buffer;
                for (int x = 320 / 4; x--;) {
                    uint8_t two_pixels = *tga_row++; // Fetch 2 pixels from TGA memory
                    uint8_t pixel1_color = two_pixels >> 4;
                    uint8_t pixel2_color = two_pixels & 15;
                    *output_buffer32++ = pixel1_color | (pixel1_color << 8) | (pixel2_color << 16) | (pixel2_color << 24);
                }
                break;
            }

            case TGA_320x200x16: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                //4bit buf with pixel doubling
                const register uint32_t *tga_row = &VIDEORAM[tga_offset + (y & 3) * 8192 + __fast_mul(y >> 2, 160)];
                for (int x = 320 / 2; x--;) {
                    const uint8_t two_pixels = *tga_row++; // Fetch 2 pixels from TGA memory
                    uint8_t pixel1 = two_pixels >> 4;
                    uint8_t pixel2 = two_pixels & 15;
                    *output_buffer++ = pixel1;
                    *output_buffer++ = pixel1;  // Pixel doubling
                    *output_buffer++ = pixel2;
                    *output_buffer++ = pixel2;  // Pixel doubling
                }
                break;
            }
            case EGA_320x200x16x4: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                const register uint32_t *ega_row = &VIDEORAM[__fast_mul(y, 40)];

                // Process 40 dwords (320 pixels) with pixel doubling
                for (int x = 0; x < 40; x++) {
                    const uint32_t ega_planes = *ega_row++;

                    // Build 8 color nibbles packed into a 32-bit word
                    const uint32_t eight_pixels = ega_pack8_from_planes(ega_planes);

                    // Unroll writing 8 pixels with horizontal doubling (16 pixels output)
                    uint8_t px = eight_pixels >> 28;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                    px = eight_pixels >> 24 & 0xF;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                    px = eight_pixels >> 20 & 0xF;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                    px = eight_pixels >> 16 & 0xF;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                    px = eight_pixels >> 12 & 0xF;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                    px = eight_pixels >> 8 & 0xF;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                    px = eight_pixels >> 4 & 0xF;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                    px = eight_pixels & 0xF;
                    *output_buffer++ = px;
                    *output_buffer++ = px;
                }
                break;
            }
            case VGA_320x200x256x4: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                const register uint32_t *vga_row = &VIDEORAM[__fast_mul(y, 80)];
                // Each byte contains one pixel, need to double each pixel
                for (int x = 0; x < 80; x++) {
                    uint32_t four_pixels = *vga_row++;  // 4 pixels
                    // Extract and double each pixel
                    uint8_t p0 = four_pixels & 0xFF;
                    uint8_t p1 = (four_pixels >> 8) & 0xFF;
                    uint8_t p2 = (four_pixels >> 16) & 0xFF;
                    uint8_t p3 = (four_pixels >> 24) & 0xFF;
                    *output_buffer++ = p0;
                    *output_buffer++ = p0;
                    *output_buffer++ = p1;
                    *output_buffer++ = p1;
                    *output_buffer++ = p2;
                    *output_buffer++ = p2;
                    *output_buffer++ = p3;
                    *output_buffer++ = p3;
                }
                break;
            }
            case VGA_320x200x256:
            default: {
                if (!in_text_area) {
                    memset(output_buffer, 0, 640);
                    break;
                }
                const uint32_t *vga_row = &VIDEORAM[__fast_mul(y, 320)];
                // Pixel doubling: each pixel written twice
                for (int x = 320; x--;) {
                    const uint8_t color = *vga_row++ & 0xFF;
                    uint8_t pixel = color >= HDMI_CTRL_BASE_INDEX ? 0 : color;
                    *output_buffer++ = pixel;
                    *output_buffer++ = pixel;  // Pixel doubling
                }
                break;
            }
        }

        // H-sync signal for visible lines (VGA standard timings)
        // VGA 640x480 timing: Front porch 16, Sync 96, Back porch 48, Active 640
        // Buffer layout: [Front 16][Sync 96][Back 48][Active 640] = 800 total
        memset(current_scanline_buffer, HDMI_CTRL_BASE_INDEX + 1, 16);   // Front porch: 16 (H Inactive, V Inactive)
        memset(current_scanline_buffer + 16, HDMI_CTRL_BASE_INDEX + 0, 96);   // H-sync: 96 (H Active, V Inactive)
        memset(current_scanline_buffer + 16 + 96, HDMI_CTRL_BASE_INDEX + 1, 48); // Back porch: 48 (H Inactive, V Inactive)
    } else {
        // VSYNC/blanking - VGA timing: 480 active, 10 front, 2 vsync, 33 back = 525 total
        const uint16_t scanline_width = 800;  // Always 800 for VGA timing
        const uint16_t vsync_start = 490;  // 480 active + 10 front porch
        const uint16_t vsync_end = 492;    // vsync_start + 2 vsync lines
        
        if (current_scanline >= vsync_start && current_scanline < vsync_end) {
            // VSync Active
            memset(current_scanline_buffer, HDMI_CTRL_BASE_INDEX + 3, 16); // Front: H Inactive, V Active
            memset(current_scanline_buffer + 16, HDMI_CTRL_BASE_INDEX + 2, 96); // Sync: H Active, V Active
            memset(current_scanline_buffer + 16 + 96, HDMI_CTRL_BASE_INDEX + 3, 48); // Back: H Inactive, V Active
            memset(current_scanline_buffer + 16 + 96 + 48, HDMI_CTRL_BASE_INDEX + 3, 640); // Rest: H Inactive, V Active
        } else {
            // VSync Inactive (Blanking)
            memset(current_scanline_buffer, HDMI_CTRL_BASE_INDEX + 1, 16); // Front: H Inactive, V Inactive
            memset(current_scanline_buffer + 16, HDMI_CTRL_BASE_INDEX + 0, 96); // Sync: H Active, V Inactive
            memset(current_scanline_buffer + 16 + 96, HDMI_CTRL_BASE_INDEX + 1, 48); // Back: H Inactive, V Inactive
            memset(current_scanline_buffer + 16 + 96 + 48, HDMI_CTRL_BASE_INDEX + 1, 640); // Rest: H Inactive, V Inactive
        }
    }
}


static inline void remove_dma_interrupt_handler() {
    irq_set_enabled(VIDEO_DMA_IRQ, false);
    irq_remove_handler(VIDEO_DMA_IRQ, irq_get_exclusive_handler(VIDEO_DMA_IRQ));
}

static inline void install_dma_interrupt_handler() {
    irq_set_exclusive_handler(VIDEO_DMA_IRQ, hdmi_scanline_interrupt_handler);
    irq_set_priority(VIDEO_DMA_IRQ, 0);
    irq_set_enabled(VIDEO_DMA_IRQ, true);
}

// Initialize HDMI output resources
static inline bool initialize_hdmi_output() {
    // Disable DMA interrupt
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_set_irq0_enabled(dma_channel_control, false);
    } else {
        dma_channel_set_irq1_enabled(dma_channel_control, false);
    }

    remove_dma_interrupt_handler();


    // Abort all DMA channels and wait for completion
    dma_hw->abort = 1 << dma_channel_control | 1 << dma_channel_data | 1 << dma_channel_palette_data | 1 << dma_channel_palette_control;

    while (dma_hw->abort) tight_loop_contents();

    // Disable state machines

#if ZERO2
    pio_set_gpio_base(PIO_VIDEO, 16);
    pio_set_gpio_base(PIO_VIDEO_ADDR, 16);
#endif

    //pio_sm_restart(PIO_VIDEO, SM_video);
    pio_sm_set_enabled(PIO_VIDEO, sm_video_output, false);

    //pio_sm_restart(PIO_VIDEO_ADDR, SM_conv);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, sm_address_converter, false);

    // Remove PIO programs
    pio_remove_program(PIO_VIDEO_ADDR, &pio_program_address_converter, pio_program_offset_converter);
    pio_remove_program(PIO_VIDEO, &pio_program_hdmi_output, pio_program_offset_video);


    pio_program_offset_converter = pio_add_program(PIO_VIDEO_ADDR, &pio_program_address_converter);
    pio_program_offset_video = pio_add_program(PIO_VIDEO, &pio_program_hdmi_output);

    pio_set_x_register(PIO_VIDEO_ADDR, sm_address_converter, (uint32_t) tmds_palette_buffer >> 12);

    // Setup control symbols (251-255) for sync signals
    uint64_t *tmds_buffer_64 = (uint64_t *) tmds_palette_buffer;
    const uint16_t ctrl_symbol_0 = 0b1101010100;
    const uint16_t ctrl_symbol_1 = 0b0010101011;
    const uint16_t ctrl_symbol_2 = 0b0101010100;
    const uint16_t ctrl_symbol_3 = 0b1010101011;

    const int base_index = HDMI_CTRL_BASE_INDEX;

    // base_index + 0: H Active (0), V Inactive (1) -> Symbol 2 (0, 1)
    tmds_buffer_64[2 * base_index + 0] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_2);
    tmds_buffer_64[2 * base_index + 1] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_2);

    // base_index + 1: H Inactive (1), V Inactive (1) -> Symbol 3 (1, 1)
    tmds_buffer_64[2 * (base_index + 1) + 0] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_3);
    tmds_buffer_64[2 * (base_index + 1) + 1] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_3);

    // base_index + 2: H Active (0), V Active (0) -> Symbol 0 (0, 0)
    tmds_buffer_64[2 * (base_index + 2) + 0] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_0);
    tmds_buffer_64[2 * (base_index + 2) + 1] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_0);

    // base_index + 3: H Inactive (1), V Active (0) -> Symbol 1 (1, 0)
    tmds_buffer_64[2 * (base_index + 3) + 0] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_1);
    tmds_buffer_64[2 * (base_index + 3) + 1] = generate_hdmi_differential_data(ctrl_symbol_0, ctrl_symbol_0, ctrl_symbol_1);

    // Configure PIO state machine for conversion

    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, pio_program_offset_converter, pio_program_offset_converter + (pio_program_address_converter.length - 1));
    sm_config_set_in_shift(&config, true, false, 32);

    pio_sm_init(PIO_VIDEO_ADDR, sm_address_converter, pio_program_offset_converter, &config);
    pio_sm_set_enabled(PIO_VIDEO_ADDR, sm_address_converter, true);

    // Configure PIO state machine for video output
    config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, pio_program_offset_video, pio_program_offset_video + (pio_program_hdmi_output.length - 1));

    sm_config_set_sideset_pins(&config,HDMI_PIN_CLOCK);
    sm_config_set_sideset(&config, 2,false,false);

    for (int i = 0; i < 2; i++) {
        pio_gpio_init(PIO_VIDEO, HDMI_PIN_CLOCK + i);
        gpio_set_drive_strength(HDMI_PIN_CLOCK + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(HDMI_PIN_CLOCK + i, GPIO_SLEW_RATE_FAST);
    }

#if ZERO2
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, sm_video_output, HDMI_BASE_PIN, 8, true);
    pio_sm_set_consecutive_pindirs(PIO_VIDEO_ADDR, sm_address_converter, HDMI_BASE_PIN, 8, true);

    uint64_t mask64 = (uint64_t)(3u << HDMI_PIN_CLOCK);
    pio_sm_set_pins_with_mask64(PIO_VIDEO, sm_video_output, mask64, mask64);
    pio_sm_set_pindirs_with_mask64(PIO_VIDEO, sm_video_output, mask64, mask64);
#else    
    pio_sm_set_pins_with_mask(PIO_VIDEO, sm_video_output, 3u << HDMI_PIN_CLOCK, 3u << HDMI_PIN_CLOCK);
    pio_sm_set_pindirs_with_mask(PIO_VIDEO, sm_video_output, 3u << HDMI_PIN_CLOCK, 3u << HDMI_PIN_CLOCK);
#endif

    for (int i = 0; i < 6; i++) {
        pio_gpio_init(PIO_VIDEO, HDMI_PIN_DATA + i);
        gpio_set_drive_strength(HDMI_PIN_DATA + i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_slew_rate(HDMI_PIN_DATA + i, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(PIO_VIDEO, sm_video_output, HDMI_PIN_DATA, 6, true);
    sm_config_set_out_pins(&config, HDMI_PIN_DATA, 6);

    sm_config_set_out_shift(&config, true, true, 30);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);

    // PIO clock divider calculation for 640x480@60Hz VGA timing:
    // - Target TMDS bit clock: 252 MHz (10× pixel clock)
    // - Pixel clock: 25.2 MHz (standard VGA 640x480@60Hz)
    // - PIO program: 10 instructions per pixel, each outputting 6 bits
    // - Each pixel requires 64 bits (2× 32-bit words with autopull at 30 bits)
    // - If clk_sys = 504 MHz: clkdiv = 504/252 = 2.0, PIO clock = 252 MHz ✓
    // - If clk_sys = 378 MHz: clkdiv = 378/252 = 1.5, PIO clock = 252 MHz ✓
    sm_config_set_clkdiv(&config, clock_get_hz(clk_sys) / 252000000.0f);
    pio_sm_init(PIO_VIDEO, sm_video_output, pio_program_offset_video, &config);
    pio_sm_set_enabled(PIO_VIDEO, sm_video_output, true);

    // Configure DMA buffers
    scanline_buffers[0] = &tmds_palette_buffer[1024];
    scanline_buffers[1] = &tmds_palette_buffer[1224];

    // VGA timing requires 800 pixels per scanline for proper sync timing
    int scanline_transfer_count = 800;

    // Configure primary data channel
    dma_channel_config dma_config = dma_channel_get_default_config(dma_channel_data);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_8);
    channel_config_set_chain_to(&dma_config, dma_channel_control); // chain to other channel

    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);

    uint dreq = (PIO_VIDEO_ADDR == pio0) ? DREQ_PIO0_TX0 + sm_address_converter : DREQ_PIO1_TX0 + sm_address_converter;

    channel_config_set_dreq(&dma_config, dreq);

    dma_channel_configure(
        dma_channel_data,
        &dma_config,
        &PIO_VIDEO_ADDR->txf[sm_address_converter], // Write address
        &scanline_buffers[0][0], // read address
        scanline_transfer_count,
        false // Don't start yet
    );

    // Configure control channel
    dma_config = dma_channel_get_default_config(dma_channel_control);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_chain_to(&dma_config, dma_channel_data); // chain to other channel

    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, false);

    dma_buffer_addresses[0] = &scanline_buffers[0][0];
    dma_buffer_addresses[1] = &scanline_buffers[1][0];

    dma_channel_configure(
        dma_channel_control,
        &dma_config,
        &dma_hw->ch[dma_channel_data].read_addr, // Write address
        &dma_buffer_addresses[0], // read address
        1, //
        false // Don't start yet
    );

    // Configure palette conversion channel

    dma_config = dma_channel_get_default_config(dma_channel_palette_data);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_chain_to(&dma_config, dma_channel_palette_control); // chain to other channel

    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);

    dreq = DREQ_PIO1_TX0 + sm_video_output;
    if (PIO_VIDEO == pio0) dreq = DREQ_PIO0_TX0 + sm_video_output;

    channel_config_set_dreq(&dma_config, dreq);

    dma_channel_configure(
        dma_channel_palette_data,
        &dma_config,
        &PIO_VIDEO->txf[sm_video_output], // Write address
        &tmds_palette_buffer[0], // read address
        2, // Transfer only 2× 32-bit words (one 64-bit pixel), skip the duplicated pixel
        false // Don't start yet
    );

    // Configure palette control channel

    dma_config = dma_channel_get_default_config(dma_channel_palette_control);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_chain_to(&dma_config, dma_channel_palette_data); // chain to other channel

    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, false);

    dreq = DREQ_PIO1_RX0 + sm_address_converter;
    if (PIO_VIDEO_ADDR == pio0) dreq = DREQ_PIO0_RX0 + sm_address_converter;

    channel_config_set_dreq(&dma_config, dreq);

    dma_channel_configure(
        dma_channel_palette_control,
        &dma_config,
        &dma_hw->ch[dma_channel_palette_data].read_addr, // Write address
        &PIO_VIDEO_ADDR->rxf[sm_address_converter], // read address
        1, //
        true // start yet
    );

    // Enable DMA interrupt and start channel
    if (VIDEO_DMA_IRQ == DMA_IRQ_0) {
        dma_channel_acknowledge_irq0(dma_channel_control);
        dma_channel_set_irq0_enabled(dma_channel_control, true);
    } else {
        dma_channel_acknowledge_irq1(dma_channel_control);
        dma_channel_set_irq1_enabled(dma_channel_control, true);
    }

    install_dma_interrupt_handler();

    dma_start_channel_mask((1u << dma_channel_control));

    return true;
}

void graphics_set_mode(enum graphics_mode_t mode) {
    graphics_mode = mode;
}

void graphics_set_palette(const uint8_t index, const uint32_t color888) {
    if (index >= HDMI_CTRL_BASE_INDEX) return; // Don't overwrite control symbols

    uint64_t *tmds_color = (uint64_t *) tmds_palette_buffer + index * 2;
    const uint8_t R = (color888 >> 16) & 0xff;
    const uint8_t G = (color888 >> 8) & 0xff;
    const uint8_t B = (color888 >> 0) & 0xff;
    tmds_color[0] = generate_hdmi_differential_data(tmds_encode_8b10b(R), tmds_encode_8b10b(G), tmds_encode_8b10b(B));
    // Invert all 60 data bits (2x30 bits) for DC balance
    // Mask: 0x3FFFFFFF3FFFFFFF (30 ones, 2 zeros, 30 ones)
    tmds_color[1] = tmds_color[0] ^ 0x3FFFFFFF3FFFFFFFULL;
}

void graphics_set_buffer(uint8_t *buffer, const uint16_t width, const uint16_t height) {
    graphics_framebuffer = buffer;
    framebuffer_width = width;
    framebuffer_height = height;
}

void graphics_init() {
    // Claim PIO state machines
    sm_video_output = pio_claim_unused_sm(PIO_VIDEO, true);
    sm_address_converter = pio_claim_unused_sm(PIO_VIDEO_ADDR, true);
    //выделение и преднастройка DMA каналов
    dma_channel_control = dma_claim_unused_channel(true);
    dma_channel_data = dma_claim_unused_channel(true);
    dma_channel_palette_control = dma_claim_unused_channel(true);
    dma_channel_palette_data = dma_claim_unused_channel(true);

    initialize_hdmi_output();
}

void graphics_set_bgcolor(uint32_t color888) {

    graphics_set_palette(255, color888);
}

void graphics_set_offset(const int x, const int y) {
    framebuffer_offset_x = x;
    framebuffer_offset_y = y;
}

void graphics_set_textbuffer(uint8_t *buffer) {
    text_buffer = buffer;
}
