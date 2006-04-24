/*
 *  makefont       create libcaca font data
 *  Copyright (c) 2006 Sam Hocevar <sam@zoy.org>
 *                All Rights Reserved
 *
 *  $Id$
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the Do What The Fuck You Want To
 *  Public License, Version 2, as published by Sam Hocevar. See
 *  http://sam.zoy.org/wtfpl/COPYING for more details.
 *
 * Usage:
 *   makefont <prefix> <font> <dpi> <bpp>
 */

#include "config.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(HAVE_ARPA_INET_H)
#   include <arpa/inet.h>
#elif defined(HAVE_NETINET_IN_H)
#   include <netinet/in.h>
#endif

#include <pango/pango.h>
#include <pango/pangoft2.h>

static int const blocklist[] =
{
    0x0000, 0x0080, /* Basic latin: A, B, C, a, img, c */
    0x0080, 0x0100, /* Latin-1 Supplement: Ä, Ç, å, ß */
    0x0100, 0x0180, /* Latin Extended-A: Ā č Ō œ */
    0x0180, 0x0250, /* Latin Extended-B: Ǝ Ƹ */
    0x0250, 0x02b0, /* IPA Extensions: ɐ ɔ ɘ ʌ ʍ */
    0x0370, 0x0400, /* Greek and Coptic: Λ α β */
    0x0400, 0x0500, /* Cyrillic: И Я */
    0x2000, 0x2070, /* General Punctuation: ‘’ “” */
#if 0
    0x2100, 0x2150, /* Letterlike Symbols: Ⅎ */
#endif
    0x2300, 0x2400, /* Miscellaneous Technical: ⌂ */
    0x2500, 0x2580, /* Box Drawing: ═ ║ ╗ ╔ ╩ */
    0x2580, 0x25a0, /* Block Elements: ▛ ▞ ░ ▒ ▓ */
    0, 0
};

static int printf_hex(char const *, uint8_t *, int);
static int printf_u32(char const *, uint32_t);
static int printf_u16(char const *, uint16_t);

int main(int argc, char *argv[])
{
    PangoContext *cx;
    PangoFontDescription *fd;
    PangoFontMap *fm;
    PangoLayout *l;
    PangoRectangle r;

    FT_Bitmap img;
    int width, height, b, i, n, blocks, glyphs;
    unsigned int glyph_size, control_size, data_size;
    uint8_t *glyph_data;

    unsigned int bpp, dpi;
    char const *prefix, *font;

    if(argc != 5)
    {
        fprintf(stderr, "%s: wrong argument count\n", argv[0]);
        fprintf(stderr, "usage: %s <prefix> <font> <dpi> <bpp>\n", argv[0]);
        fprintf(stderr, "eg: %s monospace9 \"Monospace 9\" 96 4\n", argv[0]);
        return -1;
    }

    prefix = argv[1];
    font = argv[2];
    dpi = atoi(argv[3]);
    bpp = atoi(argv[4]);

    if(dpi == 0 || (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8))
    {
        fprintf(stderr, "%s: invalid argument\n", argv[0]);
        return -1;
    }

    fprintf(stderr, "Font \"%s\", %i dpi, %i bpp\n", font, dpi, bpp);

    /* Initialise Pango */
    fm = pango_ft2_font_map_new();
    pango_ft2_font_map_set_resolution(PANGO_FT2_FONT_MAP(fm), dpi, dpi);
    cx = pango_ft2_font_map_create_context(PANGO_FT2_FONT_MAP(fm));

    l = pango_layout_new(cx);
    if(!l)
    {
        fprintf(stderr, "%s: unable to initialise pango\n", argv[0]);
        g_object_unref(cx);
        return -1;
    }

    fd = pango_font_description_from_string(font);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);

    /* Initialise our FreeType2 bitmap */
    img.width = 256;
    img.pitch = 256;
    img.rows = 256;
    img.buffer = malloc(256 * 256);
    img.num_grays = 256;
    img.pixel_mode = ft_pixel_mode_grays;

    /* Test rendering so that we know the glyph width */
    pango_layout_set_markup(l, "@", -1);
    pango_layout_get_extents(l, NULL, &r);
    width = PANGO_PIXELS(r.width);
    height = PANGO_PIXELS(r.height);
    glyph_size = ((width * height) + (8 / bpp) - 1) / (8 / bpp);
    glyph_data = malloc(glyph_size);

    /* Compute blocks and glyphs count */
    blocks = 0;
    glyphs = 0;
    for(b = 0; blocklist[b + 1]; b += 2)
    {
        blocks++;
        glyphs += blocklist[b + 1] - blocklist[b];
    }

    control_size = 24 + 12 * blocks + 8 * glyphs;
    data_size = glyph_size * glyphs;

    /* Let's go! */
    printf("/* libcucul font file\n");
    printf(" * \"%s\": %i dpi, %i bpp, %ix%i glyphs\n",
           font, dpi, bpp, width, height);
    printf(" * Automatically generated by tools/makefont.c:\n");
    printf(" *   tools/makefont %s \"%s\" %i %i\n", prefix, font, dpi, bpp);
    printf(" */\n");
    printf("\n");

    printf("static unsigned int const %s_size = %i;\n",
           prefix, 8 + control_size + data_size);
    printf("static unsigned char const %s_data[] =\n", prefix);

    printf("/* file: */\n");
    printf("\"CACA\" /* caca_header */\n");
    printf("\"FONT\" /* caca_file_type */\n");
    printf("\n");

    printf("/* font_header: */\n");
    printf_u32("\"%s\" /* control_size */\n", control_size);
    printf_u32("\"%s\" /* data_size */\n", data_size);
    printf_u16("\"%s\" /* version */\n", 1);
    printf_u16("\"%s\" /* blocks */\n", blocks);
    printf_u32("\"%s\" /* glyphs */\n", glyphs);
    printf_u16("\"%s\" /* bpp */\n", bpp);
    printf_u16("\"%s\" /* width */\n", width);
    printf_u16("\"%s\" /* height */\n", height);
    printf_u16("\"%s\" /* flags */\n", 1);
    printf("\n");

    printf("/* block_info: */\n");
    n = 0;
    for(b = 0; blocklist[b + 1]; b += 2)
    {
        printf_u32("\"%s", blocklist[b]);
        printf_u32("%s", blocklist[b + 1]);
        printf_u32("%s\"\n", n);
        n += blocklist[b + 1] - blocklist[b];
    }
    printf("\n");

    printf("/* glyph_info: */\n");
    n = 0;
    for(b = 0; blocklist[b + 1]; b += 2)
    {
        for(i = blocklist[b]; i < blocklist[b + 1]; i++)
        {
            printf_u16("\"%s", width);
            printf_u16("%s", height);
            printf_u32("%s\"\n", n * glyph_size);
            n++;
        }
    }
    printf("\n");

    printf("/* font_data: */\n");
    for(b = 0; blocklist[b + 1]; b += 2)
    {
        for(i = blocklist[b]; i < blocklist[b + 1]; i++)
        {
            unsigned int ch = i;
            char buf[10], *parser;
            int x, y, bytes;

            if(ch < 0x80)
            {
                bytes = 1;
                buf[0] = ch;
                buf[1] = '\0';
            }
            else
            {
                static const unsigned char mark[7] =
                {
                    0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC
                };

                /* FIXME: use libcucul instead of this shit */
                bytes = (ch < 0x800) ? 2 : (ch < 0x10000) ? 3 : 4;
                buf[bytes] = '\0';
                parser = buf + bytes;

                switch(bytes)
                {
                    case 4: *--parser = (ch | 0x80) & 0xbf; ch >>= 6;
                    case 3: *--parser = (ch | 0x80) & 0xbf; ch >>= 6;
                    case 2: *--parser = (ch | 0x80) & 0xbf; ch >>= 6;
                }
                *--parser = ch | mark[bytes];
            }

            /* Print glyph value in comment */
            printf("/* U+%.04X: \"", i);

            if(i < 0x20 || (i >= 0x80 && i <= 0xa0))
                printf("\\x%.02x\" */", i);
            else
                printf("%s\" */ ", buf);

            /* Render glyph on a bitmap */
            pango_layout_set_text(l, buf, -1);
            memset(glyph_data, 0, glyph_size);
            memset(img.buffer, 0, img.pitch * height);
            pango_ft2_render_layout(&img, l, 0, 0);

            /* Write bitmap as an escaped C string */
            n = 0;
            for(y = 0; y < height; y++)
            {
                for(x = 0; x < width; x++)
                {
                    uint8_t pixel = img.buffer[y * img.pitch + x];

                    pixel >>= (8 - bpp);
                    glyph_data[n / 8] |= pixel << (8 - bpp - (n % 8));
                    n += bpp;
                }
            }
            printf_hex("\"%s\"\n", glyph_data, glyph_size);
        }
    }

    printf(";\n");

    free(img.buffer);
    g_object_unref(l);
    g_object_unref(cx);

    return 0;
}

/*
 * XXX: the following functions are local
 */

static int printf_u32(char const *fmt, uint32_t i)
{
    uint32_t ni = hton32(i);
    return printf_hex(fmt, (uint8_t *)&ni, 4);
}

static int printf_u16(char const *fmt, uint16_t i)
{
    uint16_t ni = hton16(i);
    return printf_hex(fmt, (uint8_t *)&ni, 2);
}

static int printf_hex(char const *fmt, uint8_t *data, int bytes)
{
    char buf[BUFSIZ];
    char *parser = buf;
    int rewind = 0; /* we use this variable to rewind 2 bytes after \000
                     * was printed when the next char starts with "\", too. */

    while(bytes--)
    {
        uint8_t ch = *data++;
        if(ch == '\\' || ch == '"')
        {
            parser -= rewind;
            parser += sprintf(parser, "\\%c", ch);
            rewind = 0;
        }
        else if(ch >= 0x20 && ch < 0x7f)
        {
            parser += sprintf(parser, "%c", ch);
            rewind = 0;
        }
        else
        {
            parser -= rewind;
            parser += sprintf(parser, "\\%.03o", ch);
            rewind = ch ? 0 : 2;
        }
    }

    parser -= rewind;
    parser[0] = '\0';

    return printf(fmt, buf);
}

