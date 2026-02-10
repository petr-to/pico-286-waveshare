#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <signal.h>
#include <sys/time.h>
#include <cstdio>
#include "MiniFB.h"
#include "emulator/emulator.h"
#include "emulator/includes/font8x16.h"
#include "emulator/includes/font8x8.h"
#include "emu8950.h"
#include "linux-audio.h"

static uint32_t ALIGN(4, SCREEN[640 * 480]);
uint8_t ALIGN(4, DEBUG_VRAM[80 * 10]) = {0};

int cursor_blink_state = 0;
uint8_t log_debug = 0;

extern OPL *emu8950_opl;

#define AUDIO_BUFFER_LENGTH ((SOUND_FREQUENCY / 10))
static int16_t audio_buffer[AUDIO_BUFFER_LENGTH * 2] = {};
static int sample_index = 0;

extern "C" void adlib_getsample(int16_t *sndptr, intptr_t numsamples);

extern "C" void _putchar(char character) {
    putchar(character);
    static int x = 0, y = 0;

    if (y == 10) {
        y = 9;
        memmove(DEBUG_VRAM, DEBUG_VRAM + 80, 80 * 9);
        memset(DEBUG_VRAM + 80 * 9, 0, 80);
    }
    uint8_t *vidramptr = DEBUG_VRAM + y * 80 + x;

    if ((unsigned) character >= 32) {
        if (character >= 96) character -= 32;
        *vidramptr = ((character - 32) & 63) | 0 << 6;
        if (x == 80) {
            x = 0;
            y++;
        } else
            x++;
    } else if (character == '\n') {
        x = 0;
        y++;
    } else if (character == '\r') {
        x = 0;
    } else if (character == 8 && x > 0) {
        x--;
        *vidramptr = 0;
    }
}

static inline void renderer() {
    static uint8_t v = 0;
    static int render_count = 0;
    
    if (render_count == 0) {
        printf("renderer: first call, videomode=0x%02x\n", videomode);
    }
    
    if (v != videomode) {
        printf("videomode changed: 0x%02x -> 0x%02x\n", v, videomode);
        v = videomode;
    }
    
    render_count++;

    uint8_t *vidramptr = (uint8_t*)VIDEORAM + 0x8000 + ((vram_offset & 0xffff) << 1);
    uint8_t cols = 80;
    
    // Debug: check if videoram has any data
    static int debug_once = 1;
    if (debug_once && render_count == 60) {
        printf("VIDEORAM[0]=0x%08x, VIDEORAM[1]=0x%08x\n", VIDEORAM[0], VIDEORAM[1]);
        printf("vidramptr[0]=0x%02x, vidramptr[1]=0x%02x\n", vidramptr[0], vidramptr[1]);
        debug_once = 0;
    }
    
    for (int y = 0; y < 480; y++) {
        if (y >= 399)
            port3DA = 8;
        else
            port3DA = 0;

        if (y & 1)
            port3DA |= 1;

        uint32_t *pixels = SCREEN + y * 640;

        if (y < 400)
            switch (videomode) {
                case 0x00:
                case 0x01: {
                    uint16_t y_div_16 = y / 16; // Precompute y / 16
                    uint8_t glyph_line = (y / 2) % 8; // Precompute y % 8 for font lookup
                    // Calculate screen position
                    // Use uint32_t pointer to match VRAM layout (1 word per address)
                    uint32_t *text_buffer_line = VIDEORAM + 0x8000 + ((vram_offset & 0xffff) << 1) + y_div_16 * 80;

                    for (int column = 0; column < 40; column++) {
                        uint32_t char_word = *text_buffer_line++;
                        uint32_t attr_word = *text_buffer_line++;
                        
                        uint8_t char_code = char_word & 0xFF;
                        uint8_t color = attr_word & 0xFF;
                        
                        uint8_t glyph_pixels = font_8x8[char_code * 8 + glyph_line]; // Glyph row from font

                        // Cursor blinking check
                        uint8_t cursor_active = cursor_blink_state &&
                                                y_div_16 == CURSOR_Y && column == CURSOR_X &&
                                                glyph_line >= cursor_start && glyph_line <= cursor_end;

                        for (uint8_t bit = 0; bit < 8; bit++) {
                            uint8_t pixel_color;
                            if (cursor_active) {
                                pixel_color = color & 0x0F; // Cursor foreground color
                            } else if (cga_blinking && color >> 7 & 1) {
                                pixel_color = cursor_blink_state ? color >> 4 & 0x7 : color & 0x7; // Blinking background color
                            } else {
                                pixel_color = glyph_pixels >> bit & 1 ? color & 0x0f : color >> 4;
                                // Foreground or background color
                            }

                            // Write the pixel twice (horizontal scaling)
                            *pixels++ = *pixels++ = cga_palette[pixel_color];
                        }
                    }


                    break;
                }
                case 0x02:
                case 0x03: {
                    uint16_t y_div_16 = y / 16; // Precompute y / 16
                    uint8_t glyph_line = y % 16; // Precompute y % 8 for font lookup

                    // Calculate screen position
                    // Use uint32_t pointer to match VRAM layout (1 word per address)
                    uint32_t *text_row = VIDEORAM + 0x8000 + ((vram_offset & 0xffff) << 1) + y_div_16 * 160;
                    
                    for (uint8_t column = 0; column < 80; column++) {
                        // Access vidram and font data once per character
                        uint32_t *charcode = text_row + column * 2; // Character code
                        
                        uint32_t char_val = *charcode;
                        uint8_t glyph_row = font_8x16[(char_val & 0xFF) * 16 + glyph_line]; // Glyph row from font
                        
                        uint32_t attr_val = *++charcode; // Color attribute
                        uint8_t color = attr_val & 0xFF;

                        // Cursor blinking check
                        uint8_t cursor_active =
                                cursor_blink_state && y_div_16 == CURSOR_Y && column == CURSOR_X &&
                                (cursor_start > cursor_end
                                     ? !(glyph_line >= cursor_end << 1 &&
                                         glyph_line <= cursor_start << 1)
                                     : glyph_line >= cursor_start << 1 && glyph_line <= cursor_end << 1);

                        // Unrolled bit loop: Write 8 pixels with scaling (2x horizontally)
                        for (int bit = 0; bit < 8; bit++) {
                            uint8_t pixel_color;
                            if (cursor_active) {
                                pixel_color = color & 0x0F; // Cursor foreground color
                            } else if (cga_blinking && color >> 7 & 1) {
                                if (cursor_blink_state) {
                                    pixel_color = color >> 4 & 0x7; // Blinking background color
                                } else {
                                    pixel_color = glyph_row >> bit & 1 ? color & 0x0f : (color >> 4 & 0x7);
                                }
                            } else {
                                // Foreground or background color
                                pixel_color = glyph_row >> bit & 1 ? color & 0x0f : color >> 4;
                            }

                            *pixels++ = cga_palette[pixel_color];
                        }
                    }
                    break;
                }
                case 0x04:
                case 0x05: {
                    uint8_t *cga_row = vidramptr + ((y / 2 >> 1) * 80 + (y / 2 & 1) * 8192); // Precompute CGA row pointer
                    uint8_t *current_cga_palette = (uint8_t *) cga_gfxpal[cga_colorset][cga_intensity];

                    // Each byte containing 4 pixels
                    for (int x = 320 / 4; x--;) {
                        uint8_t cga_byte = *cga_row++;

                        // Extract all four 2-bit pixels from the CGA byte
                        // and write each pixel twice for horizontal scaling
                        *pixels++ = *pixels++ = cga_palette[cga_byte >> 6 & 3
                                                                ? current_cga_palette[cga_byte >> 6 & 3]
                                                                : cga_foreground_color];
                        *pixels++ = *pixels++ = cga_palette[cga_byte >> 4 & 3
                                                                ? current_cga_palette[cga_byte >> 4 & 3]
                                                                : cga_foreground_color];
                        *pixels++ = *pixels++ = cga_palette[cga_byte >> 2 & 3
                                                                ? current_cga_palette[cga_byte >> 2 & 3]
                                                                : cga_foreground_color];
                        *pixels++ = *pixels++ = cga_palette[cga_byte >> 0 & 3
                                                                ? current_cga_palette[cga_byte >> 0 & 3]
                                                                : cga_foreground_color];
                    }
                    break;
                }
                case 0x06: {
                    // Use uint32_t pointer and word offsets
                    uint32_t *cga_row = VIDEORAM + 0x8000 + ((vram_offset & 0xffff) << 1) + (y / 2 >> 1) * 80 + (y / 2 & 1) * 8192;

                    // Each byte containing 8 pixels
                    for (int x = 640 / 8; x--;) {
                        uint32_t cga_byte_val = *cga_row++;
                        uint8_t cga_byte = cga_byte_val & 0xFF;

                        *pixels++ = cga_palette[(cga_byte >> 7 & 1) * cga_foreground_color];
                        *pixels++ = cga_palette[(cga_byte >> 6 & 1) * cga_foreground_color];
                        *pixels++ = cga_palette[(cga_byte >> 5 & 1) * cga_foreground_color];
                        *pixels++ = cga_palette[(cga_byte >> 4 & 1) * cga_foreground_color];
                        *pixels++ = cga_palette[(cga_byte >> 3 & 1) * cga_foreground_color];
                        *pixels++ = cga_palette[(cga_byte >> 2 & 1) * cga_foreground_color];
                        *pixels++ = cga_palette[(cga_byte >> 1 & 1) * cga_foreground_color];
                        *pixels++ = cga_palette[(cga_byte >> 0 & 1) * cga_foreground_color];
                    }

                    break;
                }
                case 0x1e:
                    cols = 90;
                    vram_offset = 5;
                    if (y >= 348) break;
                case 0x7: {
                    uint8_t *cga_row = vram_offset + (uint8_t*)VIDEORAM + (y & 3) * 8192 + y / 4 * cols;
                    // Each byte containing 8 pixels
                    for (int x = 640 / 8; x--;) {
                        uint8_t cga_byte = *cga_row++;

                        *pixels++ = cga_palette[(cga_byte >> 7 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 6 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 5 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 4 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 3 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 2 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 1 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 0 & 1) * 15];
                    }

                    break;
                }

                case 0x8:
                case 0x74: /* 160x200x16    */
                case 0x76: /* cga composite / tandy */ {
                    uint32_t *palette;
                    switch (videomode) {
                        case 0x08:
                            palette = tga_palette;
                            break;
                        case 0x74:
                            palette = cga_composite_palette[cga_intensity << 1];
                            break;
                        case 0x76:
                            palette = cga_composite_palette[0];
                            break;
                    }

                    // Use uint32_t pointer and correct tga_offset
                    uint32_t *cga_row = VIDEORAM + tga_offset + (y / 2 >> 1) * 80 + (y / 2 & 1) * 8192; 

                    // Each byte containing 8 pixels
                    for (int x = 640 / 8; x--;) {    
                        uint32_t cga_byte_val = *cga_row++; // Fetch 8 pixels from TGA memory
                        uint8_t cga_byte = cga_byte_val & 0xFF;
                        
                        uint8_t color1 = cga_byte >> 4 & 15;
                        uint8_t color2 = cga_byte & 15;

                        if (!color1 && videomode == 0x8) color1 = cga_foreground_color;
                        if (!color2 && videomode == 0x8) color2 = cga_foreground_color;

                        *pixels++ = *pixels++ = *pixels++ = *pixels++ = palette[color1];
                        *pixels++ = *pixels++ = *pixels++ = *pixels++ = palette[color2];
                    }

                    break;
                }
                case 0x09: /* tandy 320x200 16 color */ {
                    // Use uint32_t pointer
                    uint32_t *tga_row = VIDEORAM + tga_offset + (y / 2 & 3) * 8192 + y / 8 * 160;

                    // Each byte containing 4 pixels
                    for (int x = 320 / 2; x--;) {
                        uint32_t tga_byte_val = *tga_row++;
                        uint8_t tga_byte = tga_byte_val & 0xFF;
                        
                        *pixels++ = *pixels++ = tga_palette[tga_palette_map[tga_byte >> 4 & 15]];
                        *pixels++ = *pixels++ = tga_palette[tga_palette_map[tga_byte & 15]];
                    }
                    break;
                }
                case 0x0a: /* tandy 640x200 16 color */ {
                    // Use uint32_t pointer, note: this mode typically starts at B8000, 
                    // ensure offset logic matches. tga_offset usually handles bank switching for Tandy.
                    // Assuming flat mapping relative to tga_offset or 0x8000 if not set.
                    // Safe guess based on other modes: use 0x8000 + offset if tga_offset is not used?
                    // Previous code was VIDEORAM + (y/2)*320. If that meant 0 offset, it accessed A0000.
                    // If A0000 is used for this mode, fine. If not, it might need 0x8000.
                    // Let's stick to simple type fix for now to match the pattern.
                    uint32_t *tga_row = VIDEORAM + y / 2 * 320; 

                    // Each byte contains 2 pixels
                    for (int x = 640 / 2; x--;) {
                        uint32_t tga_byte_val = *tga_row++;
                        uint8_t tga_byte = tga_byte_val & 0xFF;
                        
                        *pixels++ = tga_palette[tga_palette_map[tga_byte >> 4 & 15]];
                        *pixels++ = tga_palette[tga_palette_map[tga_byte & 15]];
                    }
                    break;
                }
                case 0x0D: /* EGA 320x200 16-color */ {
                    if (y >= 400) break;
                    uint32_t* vram_ptr = &VIDEORAM[(y / 2) * (320 / 8)];
                    for (int i = 0; i < (320 / 8); ++i) {
                        uint32_t eight_pixels = vram_ptr[i];
                        uint8_t plane0 =  eight_pixels        & 0xFF;
                        uint8_t plane1 = (eight_pixels >> 8)  & 0xFF;
                        uint8_t plane2 = (eight_pixels >> 16) & 0xFF;
                        uint8_t plane3 = (eight_pixels >> 24) & 0xFF;

                        for (int bit = 7; bit >= 0; --bit) {
                            uint8_t color_index = ((plane0 >> bit) & 1)
                                                | (((plane1 >> bit) & 1) << 1)
                                                | (((plane2 >> bit) & 1) << 2)
                                                | (((plane3 >> bit) & 1) << 3);
                            uint32_t color = vga_palette[color_index];
                            *pixels++ = color;
                            *pixels++ = color;
                        }
                    }
                    break;
                }
                case 0x0E: /* EGA 640x200 16-color */ {
                    if (y >= 400) break;
                    uint32_t* vram_ptr = &VIDEORAM[(y / 2) * (640 / 8)];
                    for (int i = 0; i < (640 / 8); ++i) {
                        uint32_t eight_pixels = vram_ptr[i];
                        uint8_t plane0 =  eight_pixels        & 0xFF;
                        uint8_t plane1 = (eight_pixels >> 8)  & 0xFF;
                        uint8_t plane2 = (eight_pixels >> 16) & 0xFF;
                        uint8_t plane3 = (eight_pixels >> 24) & 0xFF;

                        for (int bit = 7; bit >= 0; --bit) {
                            uint8_t color_index = ((plane0 >> bit) & 1)
                                                | (((plane1 >> bit) & 1) << 1)
                                                | (((plane2 >> bit) & 1) << 2)
                                                | (((plane3 >> bit) & 1) << 3);
                            *pixels++ = vga_palette[color_index];
                        }
                    }
                    break;
                }
                case 0x10: /* EGA 640x350 16-color */ {
                    if (y >= 350) break;
                    uint32_t* vram_ptr = &VIDEORAM[y * (640 / 8)];
                    for (int i = 0; i < (640 / 8); ++i) {
                        uint32_t eight_pixels = vram_ptr[i];
                        uint8_t plane0 =  eight_pixels        & 0xFF;
                        uint8_t plane1 = (eight_pixels >> 8)  & 0xFF;
                        uint8_t plane2 = (eight_pixels >> 16) & 0xFF;
                        uint8_t plane3 = (eight_pixels >> 24) & 0xFF;

                        for (int bit = 7; bit >= 0; --bit) {
                            uint8_t color_index = ((plane0 >> bit) & 1)
                                                | (((plane1 >> bit) & 1) << 1)
                                                | (((plane2 >> bit) & 1) << 2)
                                                | (((plane3 >> bit) & 1) << 3);
                            *pixels++ = vga_palette[color_index];
                        }
                    }
                    break;
                }
                case 0x11: /* VGA 640x480 2-color */ {
                    uint8_t *cga_row = (uint8_t*)VIDEORAM + y * 80;
                    // Each byte containing 8 pixels
                    for (int x = 640 / 8; x--;) {
                        uint8_t cga_byte = *cga_row++;

                        *pixels++ = cga_palette[(cga_byte >> 7 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 6 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 5 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 4 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 3 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 2 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 1 & 1) * 15];
                        *pixels++ = cga_palette[(cga_byte >> 0 & 1) * 15];
                    }

                    break;
                }
                case 0x12: /* VGA 640x480 16-color */ {
                    if (y >= 480) break;
                    uint32_t* vram_ptr = &VIDEORAM[y * (640 / 8)];
                    for (int i = 0; i < (640 / 8); ++i) {
                        uint32_t eight_pixels = vram_ptr[i];
                        uint8_t plane0 =  eight_pixels        & 0xFF;
                        uint8_t plane1 = (eight_pixels >> 8)  & 0xFF;
                        uint8_t plane2 = (eight_pixels >> 16) & 0xFF;
                        uint8_t plane3 = (eight_pixels >> 24) & 0xFF;

                        for (int bit = 7; bit >= 0; --bit) {
                            uint8_t color_index = ((plane0 >> bit) & 1)
                                                | (((plane1 >> bit) & 1) << 1)
                                                | (((plane2 >> bit) & 1) << 2)
                                                | (((plane3 >> bit) & 1) << 3);
                            *pixels++ = vga_palette[color_index];
                        }
                    }
                    break;
                }
                case 0x13: {
                    if (vga_planar_mode) {
                        for (int x = 0; x < 320; x++) {
                            uint32_t ptr = x + (y >> 1) * 320;
                            ptr = (ptr >> 2) + (x & 3) * vga_plane_size;
                            ptr += vram_offset;
                            uint32_t color = vga_palette[VIDEORAM[ptr]];
                            *pixels++ = *pixels++ = color;
                        }
                    } else {
                        // Standard chain-4 mode
                        uint32_t *vga_row = VIDEORAM + (y >> 1) * 320;
                        for (int x = 0; x < 320; x++) {
                            uint32_t val = *vga_row++;
                            uint32_t color = vga_palette[val & 0xFF];
                            *pixels++ = *pixels++ = color;
                        }
                    }

                    break;
                }
                case 0x78: /* 80x100x16 textmode */
                    cols = 40;
                case 0x77: /* 160x100x16 textmode */ {
                    uint16_t y_div_4 = y / 4; // Precompute y / 4
                    uint8_t odd_even = y / 2 & 1;
                    // Calculate screen position
                    // Use uint32_t pointer
                    uint32_t *cga_row = VIDEORAM + 0x8000 + ((vram_offset & 0xffff) << 1) + y_div_4 * 160;
                    
                    for (uint8_t column = 0; column < cols; column++) {
                        // Access vidram and font data once per character
                        uint32_t *charcode = cga_row + column * 2; // Character code
                        uint32_t char_val = *charcode;
                        uint32_t attr_val = *++charcode;
                        
                        uint8_t glyph_row = font_8x8[(char_val & 0xFF) * 8 + odd_even]; // Glyph row from font
                        uint8_t color = attr_val & 0xFF;

#pragma GCC unroll(8)
                        for (uint8_t bit = 0; bit < 8; bit++) {
                            *pixels++ = cga_palette[glyph_row >> bit & 1 ? color & 0x0f : color >> 4];
                        }
                    }
                    break;
                }
                case 0x79: /* 80x200x16 textmode */ {
                    int y_div_2 = y / 2; // Precompute y / 2
                    // Calculate screen position
                    // Use uint32_t pointer
                    uint32_t *cga_row = VIDEORAM + 0x8000 + ((vram_offset & 0xffff) << 1) + y_div_2 * 80 + (y_div_2 & 1 * 8192);

                    for (int column = 0; column < 40; column++) {
                        // Access vidram and font data once per character
                        uint32_t *charcode = cga_row + column * 2; // Character code
                        
                        uint32_t char_val = *charcode;
                        uint32_t attr_val = *++charcode;
                        
                        uint8_t glyph_row = font_8x8[(char_val & 0xFF) * 8]; // Glyph row from font
                        uint8_t color = attr_val & 0xFF;

#pragma GCC unroll(8)
                        for (int bit = 0; bit < 8; bit++) {
                            *pixels++ = *pixels++ = cga_palette[glyph_row >> bit & 1 ? color & 0x0f : color >> 4];
                        }
                    }
                    break;
                }
                case 0x87: {
                    /* 40x46 ??? */
                    int y_div_2 = y / 8; // Precompute y / 2
                    // Calculate screen position
                    // Use uint32_t pointer
                    uint32_t *cga_row = VIDEORAM + 0x8000 + ((vram_offset & 0xffff) << 1) + y_div_2 * 80 + (y_div_2 & 1 * 8192);
                    
                    for (int column = 0; column < 40; column++) {
                        // Access vidram and font data once per character
                        uint32_t *charcode = cga_row + column * 2; // Character code
                        
                        uint32_t char_val = *charcode;
                        uint32_t attr_val = *++charcode;
                        
                        uint8_t glyph_row = font_8x8[(char_val & 0xFF) * 8 + (y_div_2 % 8)]; // Glyph row from font
                        uint8_t color = attr_val & 0xFF;

#pragma GCC unroll(8)
                        for (int bit = 0; bit < 8; bit++) {
                            *pixels++ = *pixels++ = cga_palette[glyph_row >> bit & 1 ? color & 0x0f : color >> 4];
                        }
                    }
                    break;
                }
                default:
                    printf("Unsupported videomode %x\n", videomode);
                    break;
            }
        else {
            uint8_t ydebug = y - 400;
            uint8_t y_div_8 = ydebug / 8;
            uint8_t glyph_line = ydebug % 8;

            const uint8_t colors[4] = {0x0f, 0xf0, 10, 12};
            // Pointer to character data
            uint8_t *text_buffer_line = &DEBUG_VRAM[y_div_8 * 80];
            for (uint8_t column = 80; column--;) {
                const uint8_t character = *text_buffer_line++;
                const uint8_t color = colors[character >> 6];
                uint8_t glyph_pixels = font_8x8[(32 + (character & 63)) * 8 + glyph_line];
                // Read from fast palette lookup table for 2-bit color combinations
                // Unrolled bit loop: Write 8 pixels with scaling (2x horizontally)
                for (int bit = 0; bit < 8; bit++) {
                    *pixels++ = cga_palette[glyph_pixels >> bit & 1 ? color & 0x0f : color >> 4];
                }
            }
        }
    }
}

extern "C" void HandleInput(unsigned int keycode, int isKeyDown) {
    // Convert X11 keycode to PC scancode
    unsigned char scancode = 0;

    switch (keycode) {
        case 27: scancode = 0x01;
            break; // Escape
        case 49: scancode = 0x02;
            break; // 1
        case 50: scancode = 0x03;
            break; // 2
        case 51: scancode = 0x04;
            break; // 3
        case 52: scancode = 0x05;
            break; // 4
        case 53: scancode = 0x06;
            break; // 5
        case 54: scancode = 0x07;
            break; // 6
        case 55: scancode = 0x08;
            break; // 7
        case 56: scancode = 0x09;
            break; // 8
        case 57: scancode = 0x0A;
            break; // 9
        case 48: scancode = 0x0B;
            break; // 0
        case 65: scancode = 0x1E;
            break; // A
        case 66: scancode = 0x30;
            break; // B
        case 67: scancode = 0x2E;
            break; // C
        case 68: scancode = 0x20;
            break; // D
        case 69: scancode = 0x12;
            break; // E
        case 70: scancode = 0x21;
            break; // F
        case 71: scancode = 0x22;
            break; // G
        case 72: scancode = 0x23;
            break; // H
        case 73: scancode = 0x17;
            break; // I
        case 74: scancode = 0x24;
            break; // J
        case 75: scancode = 0x25;
            break; // K
        case 76: scancode = 0x26;
            break; // L
        case 77: scancode = 0x32;
            break; // M
        case 78: scancode = 0x31;
            break; // N
        case 79: scancode = 0x18;
            break; // O
        case 80: scancode = 0x19;
            break; // P
        case 81: scancode = 0x10;
            break; // Q
        case 82: scancode = 0x13;
            break; // R
        case 83: scancode = 0x1F;
            break; // S
        case 84: scancode = 0x14;
            break; // T
        case 85: scancode = 0x16;
            break; // U
        case 86: scancode = 0x2F;
            break; // V
        case 87: scancode = 0x11;
            break; // W
        case 88: scancode = 0x2D;
            break; // X
        case 89: scancode = 0x15;
            break; // Y
        case 90: scancode = 0x2C;
            break; // Z
        case 32: scancode = 0x39;
            break; // Space
        case 13: scancode = 0x1C;
            break; // Enter
        case 8: scancode = 0x0E;
            break; // Backspace
        case 9: scancode = 0x0F;
            break; // Tab
        case 1000: scancode = 0x4B;
            break; // Left
        case 1001: scancode = 0x48;
            break; // Up
        case 1002: scancode = 0x4D;
            break; // Right
        case 1003: scancode = 0x50;
            break; // Down
        case 1004: scancode = 0x3B;
            break; // F1
        case 1005: scancode = 0x3C;
            break; // F2
        case 1006: scancode = 0x3D;
            break; // F3
        case 1007: scancode = 0x3E;
            break; // F4
        case 1008: scancode = 0x3F;
            break; // F5
        case 1009: scancode = 0x40;
            break; // F6
        case 1010: scancode = 0x41;
            break; // F7
        case 1011: scancode = 0x42;
            break; // F8
        case 1012: scancode = 0x43;
            break; // F9
        case 1013: scancode = 0x44;
            break; // F10
        case 1014: scancode = 0x57;
            break; // F11
        case 1015: scancode = 0x58;
            break; // F12
        case 16: scancode = 0x2A;
            break; // Shift
        case 17: scancode = 0x1D;
            break; // Ctrl
        case 18: scancode = 0x38;
            break; // Alt
        
        // Punctuation
        case 59: // ;
        case 58: // :
            scancode = 0x27; break;
        case 39: // '
        case 34: // "
            scancode = 0x28; break;
        case 44: // ,
        case 60: // <
            scancode = 0x33; break;
        case 46: // .
        case 62: // >
            scancode = 0x34; break;
        case 47: // /
        case 63: // ?
            scancode = 0x35; break;
        case 91: // [
        case 123: // {
            scancode = 0x1A; break;
        case 93: // ]
        case 125: // }
            scancode = 0x1B; break;
        case 45: // -
        case 95: // _
            scancode = 0x0C; break;
        case 61: // =
        case 43: // +
            scancode = 0x0D; break;
        case 92: // \
        case 124: // |
            scancode = 0x2B; break;
        case 96: // `
        case 126: // ~
            scancode = 0x29; break;

        default: scancode = 0;
            break;
    }

    if (!isKeyDown && scancode != 0) {
        scancode |= 0x80;
    }

    port60 = scancode;
    port64 |= 2;
    doirq(1);
}

extern "C" void HandleMouse(int x, int y, int buttons) {
    static int prev_x = 0, prev_y = 0;
    sermouseevent(buttons, x - prev_x, y - prev_y);
    prev_y = y;
    prev_x = x;
}

extern "C" int HanldeMenu(int menu_id, int checked) {
    switch (menu_id) {
        case 1:
            return !checked;
        case 2:
            return !checked;
        default:
            return 0;
    }
}

static volatile int running = 1;

void signal_handler(int sig) {
    running = 0;
}

pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t update_cond = PTHREAD_COND_INITIALIZER;
volatile int update_ready = 0;

void *sound_thread(void *arg) {
    while (running) {
        pthread_mutex_lock(&update_mutex);
        while (!update_ready && running) {
            pthread_cond_wait(&update_cond, &update_mutex);
        }
        if (!running) {
            pthread_mutex_unlock(&update_mutex);
            break;
        }
        update_ready = 0;
        pthread_mutex_unlock(&update_mutex);

        // Send audio buffer to Linux audio system
        if (linux_audio_write(audio_buffer, AUDIO_BUFFER_LENGTH) != 0) {
            // Audio write failed, but continue running
            // printf("Audio write failed!\n");
            // usleep(1000); // 1ms delay
        }
    }
    return NULL;
}

void *ticks_thread(void *arg) {
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t elapsed_system_timer = 0;
    uint64_t elapsed_blink_tics = 0;
    uint64_t elapsed_frame_tics = 0;
    uint64_t last_dss_tick = 0;
    uint64_t last_sb_tick = 0;
    uint64_t last_sound_tick = 0;

    int16_t last_dss_sample = 0;
    int16_t last_sb_sample = 0;

    const uint64_t hostfreq = 1000000000; // nanoseconds

    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &current);

        uint64_t elapsedTime = (current.tv_sec - start.tv_sec) * hostfreq + (current.tv_nsec - start.tv_nsec);

        // Timer interrupt (~18.2 Hz)
        if (elapsedTime - elapsed_system_timer >= hostfreq / timer_period) {
            doirq(0);
            elapsed_system_timer = elapsedTime;
        }

        // Disney Sound Source frequency ~7KHz
        if (elapsedTime - last_dss_tick >= hostfreq / 7000) {
            last_dss_sample = dss_sample();
            last_dss_tick = elapsedTime;
        }

        // Sound Blaster
        if (elapsedTime - last_sb_tick >= hostfreq / 22050) {
            last_sb_sample = blaster_sample();
            last_sb_tick = elapsedTime;
        }

        // Audio samples
        if (elapsedTime - last_sound_tick >= hostfreq / SOUND_FREQUENCY) {
            get_sound_sample(last_dss_sample + last_sb_sample, &audio_buffer[sample_index]);
            sample_index += 2;
            

            if (sample_index >= AUDIO_BUFFER_LENGTH * 2) {
                pthread_mutex_lock(&update_mutex);
                update_ready = 1;
                pthread_cond_signal(&update_cond);
                pthread_mutex_unlock(&update_mutex);
                sample_index = 0;
            }

            last_sound_tick = elapsedTime;
        }

        // Cursor blink
        if (elapsedTime - elapsed_blink_tics >= 333333333) {
            // ~3Hz
            cursor_blink_state ^= 1;
            elapsed_blink_tics = elapsedTime;
        }

        // Frame rendering (60 FPS)
        if (elapsedTime - elapsed_frame_tics >= 16666666) {
            // ~60Hz
            renderer();
            elapsed_frame_tics = elapsedTime;
        }

        // No sleep - let the timing be controlled by clock precision
    }
    return NULL;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("Opening window...\n");
    fflush(stdout);
    
    if (!mfb_open("Pico-286 Emulator", 640, 480, 1)) {
        printf("Failed to open window\n");
        return -1;
    }
    
    printf("Window opened successfully!\n");
    fflush(stdout);

    // Initialize memory access functions (required before reset86!)
    write86 = write86_ob;
    writew86 = writew86_ob;
    writedw86 = writedw86_ob;
    read86 = read86_ob;
    readw86 = readw86_ob;
    readdw86 = readdw86_ob;

    // Test: fill screen with blue to verify rendering works
    for (int i = 0; i < 640 * 480; i++) {
        SCREEN[i] = 0x0000FF;  // Blue
    }
    
    emu8950_opl = OPL_new(3579552, SOUND_FREQUENCY);
    blaster_reset();
    sn76489_reset();
    reset86();
    
    // Test: Write some text to video RAM after reset
    // Video RAM starts at offset 0xB8000 - 0xA0000 = 0x18000 bytes into VIDEORAM
    // For text mode 80x25, we write character + attribute pairs
    uint8_t *textram = (uint8_t*)VIDEORAM + 0x18000; // 0xB8000 area
    const char *test_msg = "HELLO FROM EMULATOR!";
    for (int i = 0; test_msg[i]; i++) {
        textram[i * 2] = test_msg[i];      // Character
        textram[i * 2 + 1] = 0x0F;         // White on black
    }
    printf("Test text written to VIDEORAM at offset 0x18000\n");

    // Initialize audio system
    if (linux_audio_init(SOUND_FREQUENCY, 2, AUDIO_BUFFER_LENGTH) == 0) {
        if (linux_audio_start() == 0) {
            printf("Audio: %s backend started\n", linux_audio_get_backend_name());
        } else {
            printf("Audio: Failed to start, continuing without audio\n");
        }
    } else {
        printf("Audio: Failed to initialize, continuing without audio\n");
    }

    pthread_t sound_tid, ticks_tid;
    pthread_create(&sound_tid, NULL, sound_thread, NULL);
    pthread_create(&ticks_tid, NULL, ticks_thread, NULL);

    printf("Starting main loop...\n");
    fflush(stdout);
    
    int frame_count = 0;
    while (running) {
        exec86(32768);  // Reduced from 32768 to allow more frequent audio updates
        
        if (frame_count == 0) {
            printf("First mfb_update call...\n");
            fflush(stdout);
        }
        
        if (mfb_update(SCREEN, 0) < 0) {
            printf("mfb_update failed, exiting\n");
            running = 0;
            break;
        }
        
        frame_count++;
        if (frame_count == 60) {
            printf("60 frames rendered\n");
            fflush(stdout);
        }
    }

    pthread_cancel(sound_tid);
    pthread_cancel(ticks_tid);
    pthread_join(sound_tid, NULL);
    pthread_join(ticks_tid, NULL);

    // Clean up audio
    linux_audio_close();

    mfb_close();
    return 0;
}
