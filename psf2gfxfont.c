#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "gfxfont.h"
#include "psf.h"

// Simple tool for converting PSF fonts to Adafruit GFX fonts.
// Graham Knap, February 2022

#define PSF_BUFFER_SIZE 32768

typedef struct PSF_T
{
    uint8_t data[PSF_BUFFER_SIZE];
    uint32_t dataLen;
    uint16_t format;        // 1 = PSF1, 2 = PSF2
    uint16_t glyphDataOffset;
    uint16_t count;         // number of glyphs
    uint16_t height;        // glyph height, in pixels
    uint16_t width;         // glyph width, in pixels
    uint16_t widthBytes;    // glyph width, in bytes
    uint16_t charSize;      // bytes per character
    uint32_t unicodeTableOffset;
} PSF;

bool LoadPsf(FILE * f, PSF * p)
{
    bool haveUnicodeTable;
    
    p->dataLen = fread(p->data, 1, PSF_BUFFER_SIZE, f);
    
    if (p->dataLen < sizeof(psf1_header))
    {
        fprintf(stderr, "Incomplete header\n");
        return false;
    }
    
    if (p->dataLen == PSF_BUFFER_SIZE
        && fread(p->data, 1, 1, f) > 0)
    {
        fprintf(stderr, "PSF data is too large\n");
        return false;
    }
    
    if (p->data[0] == PSF1_MAGIC0 && p->data[1] == PSF1_MAGIC1)
    {
        psf1_header * h = (psf1_header *)p->data;
        p->format = 1;
        p->glyphDataOffset = sizeof(psf1_header);
        p->count = (h->mode & PSF1_MODE512) ? 512 : 256;
        p->height = h->charsize;
        p->width = 8;
        p->widthBytes = 1;
        p->charSize = p->height * p->widthBytes;
        haveUnicodeTable = (h->mode & PSF1_MODEHASTAB);
    }
    else if (p->data[0] == PSF2_MAGIC0 && p->data[1] == PSF2_MAGIC1
        && p->data[2] == PSF2_MAGIC2 && p->data[3] == PSF2_MAGIC3)
    {
        if (p->dataLen < sizeof(psf2_header))
        {
            fprintf(stderr, "Incomplete psf2 header\n");
            return false;
        }
        
        psf2_header * h = (psf2_header *)p->data;
        p->format = 2;
        p->glyphDataOffset = h->headersize;
        p->count = h->length;
        p->height = h->height;
        p->width = h->width;
        p->widthBytes = (h->width + 7) / 8;
        haveUnicodeTable = (h->flags & PSF2_HAS_UNICODE_TABLE);
    }
    else
    {
        fprintf(stderr, "Unrecognized magic\n");
        return false;
    }
    
    // read glyph data
    p->charSize = p->height * p->widthBytes;
    uint32_t glyphDataSize = p->count * p->charSize;
    
    if (p->glyphDataOffset + glyphDataSize > p->dataLen)
    {
        fprintf(stderr, "Incomplete glyph data\n");
        return false;
    }
    
    p->unicodeTableOffset = (haveUnicodeTable) ? p->glyphDataOffset + glyphDataSize : 0;
    return true;
}

bool DisplayGlyph(const PSF * p, uint16_t index)
{
    if (p == NULL || index > p->count)
    {
        return false;
    }
    
    uint32_t offset = p->glyphDataOffset + (index * p->charSize);
    
    for (int row = 0; row < p->height; row++)
    {
        uint8_t x;

        for (uint32_t col = 0; col < p->width; col++)
        {
            if ((col & 7) == 0)
            {
                x = p->data[offset];
                offset++;
            }
            
            fputc(x & 128 ? 'X' : '.', stdout);
            x <<= 1;
        }
        
        fputc('\n', stdout);
    }
    
    return true;
}

uint32_t FindUnicodeEntryForGlyph(const PSF * p, uint16_t glyphIndex)
{
    if (p == NULL || glyphIndex > p->count || p->unicodeTableOffset == 0)
    {
        return 0;
    }
    
    uint32_t offset = p->unicodeTableOffset;
    
    if (p->format == 1)
    {
        for (uint16_t g = 0; g < glyphIndex; g++)
        {
            uint16_t x = 0;
            
            while (x != PSF1_SEPARATOR)
            {
                if (offset + 1 >= p->dataLen)
                {
                    fprintf(stderr, "Incomplete unicode table\n");
                    return 0;
                }
                
                memcpy(&x, p->data + offset, 2);
                offset += 2;
            }
        }
    }
    else
    {
        for (uint16_t g = 0; g < glyphIndex; g++)
        {
            uint8_t x = 0;
            
            while (x != PSF2_SEPARATOR)
            {
                if (offset >= p->dataLen)
                {
                    fprintf(stderr, "Incomplete unicode table\n");
                    return 0;
                }
                
                x = p->data[offset];
                offset++;
            }
        }
    }
    
    return offset;
}

uint16_t FindGlyphForUnicode(const PSF * p, uint16_t codePoint)
{
    if (p == NULL || p->format != 1 || p->unicodeTableOffset == 0)
    {
        return UINT16_MAX;
    }
    
    uint32_t offset = p->unicodeTableOffset;
    uint16_t glyph = 0;
    
    while (offset + 1 < p->dataLen)
    {
        uint16_t x;
        memcpy(&x, p->data + offset, 2);
        if (x == codePoint) return glyph;
        if (x == PSF1_SEPARATOR) glyph++;
        offset += 2;
    }
    
    return UINT16_MAX;
}

uint16_t Utf8CharLength(const uint8_t * u)
{
    return (u[0] & 0x80) == 0 ? 1
        : (u[0] & 0xe0) == 0xc0 ? 2
        : (u[0] & 0xf0) == 0xe0 ? 3
        : (u[0] & 0xf8) == 0xf0 ? 4
        : 0;
}

uint16_t FindGlyphForUtf8(const PSF * p, const uint8_t * u)
{
    if (p == NULL || p->format == 1 || p->unicodeTableOffset == 0)
    {
        return UINT16_MAX;
    }
    
    uint16_t len = Utf8CharLength(u);
    if (len == 0) return UINT16_MAX;
    
    uint32_t offset = p->unicodeTableOffset;
    uint16_t glyph = 0;
    
    while (offset + len <= p->dataLen)
    {
        if (p->data[offset] == PSF2_SEPARATOR)
        {
            glyph++;
            offset++;
        }
        else if (memcmp(u, p->data + offset, len) == 0)
        {
            return glyph;
        }
        else
        {
            uint16_t n = Utf8CharLength(p->data + offset);
            if (n == 0) return UINT16_MAX;
            offset += n;
        }
    }
    
    return UINT16_MAX;
}

void ShowUsageMessage(void)
{
    fprintf(stderr, "Usage: psf2gfxfont [options]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -f psfFileName   input file, '-' for stdin\n");
    fprintf(stderr, "  -g gfxFontName   name of GFXfont structure\n");
    fprintf(stderr, "  -o gfxFileName   output file, '-' for stdout\n");
}

int main(int argc, char * argv[])
{
    FILE * inputFile = NULL;
    PSF psf;
    int opt;
    const char * inputFileName = NULL;
    const char * gfxFontName = NULL;
    const char * outputFileName = NULL;
    
    while ((opt = getopt(argc, argv, "+f:g:o:")) != -1)
    {
        switch (opt)
        {
            case 'f':
                inputFileName = optarg;
                break;
                
            case 'g':
                gfxFontName = optarg;
                break;
                
            case 'o':
                outputFileName = optarg;
                break;
                
            default:
                ShowUsageMessage();
                return 1;
        }
    }

    if (inputFileName == NULL)
    {
        fprintf(stderr, "Input file not specified\n");
        return 1;
    }
    
    if (gfxFontName == NULL)
    {
        fprintf(stderr, "GFX font name not specified\n");
        return 1;
    }
    
    if (outputFileName == NULL)
    {
        fprintf(stderr, "Output file not specified\n");
        return 1;
    }

    // load the PSF font
    if (strcmp(inputFileName, "-") == 0)
    {    
        inputFile = stdin;
    }
    else
    {
        inputFile = fopen(inputFileName, "r");
    }
    
    if (inputFile == NULL)
    {
        fprintf(stderr, "Failed to open input file\n");
        return 1;
    }
    
    if (!LoadPsf(inputFile, &psf))
    {
        return 1;
    }
    
    if (inputFile != stdin)
    {
        fclose(inputFile);
    }
    
    inputFile = NULL;

    // create the GFX font
    FILE * outputFile = NULL;

    if (strcmp(outputFileName, "-") == 0)
    {
        outputFile = stdout;
    }
    else
    {
        outputFile = fopen(outputFileName, "w");
    }
    
    if (outputFile == NULL)
    {
        fprintf(stderr, "Failed to open output file\n");
        return 1;
    }
    
    const uint16_t maxPsfByteWidth = 4;
    
    if (psf.widthBytes > maxPsfByteWidth)
    {
        fprintf(stderr, "PSF width is too large\n");
        return 1;
    }
    
    const uint16_t first = 32;
    const uint16_t last = 126;
    GFXglyph gfxGlyph[last - first + 1];
    
    fprintf(outputFile, "const uint8_t %sBitmaps[] PROGMEM = {\n", gfxFontName);
    uint16_t bitmapOffset = 0;

    for (uint16_t i = 0; i < last - first + 1; i++)
    {
        uint16_t charCode = first + i;
        fprintf(outputFile, "    /* '%c' */", charCode);
        uint16_t psfOffset = psf.glyphDataOffset + (charCode * psf.charSize);
        gfxGlyph[i].bitmapOffset = bitmapOffset;
        
        // determine crop dimensions
        uint8_t startRow = UINT8_MAX, endRow = UINT8_MAX;
        uint8_t colMask[maxPsfByteWidth];
        memset(colMask, 0, sizeof(colMask));
        uint16_t p = psfOffset;

        for (uint16_t row = 0; row < psf.height; row++)
        {
            bool rowUsed = false;
            
            for (uint16_t col = 0; col < psf.widthBytes; col++)
            {
                rowUsed |= (psf.data[p] != 0);
                colMask[col] |= psf.data[p];
                p++;
            }
            
            if (rowUsed)
            {
                if (startRow == UINT8_MAX)
                {
                    startRow = row;
                    endRow = row;
                }
                else
                {
                    endRow = row;
                }
            }
        }
        
        if (startRow == UINT8_MAX)
        {
            gfxGlyph[i].width = 0;
            gfxGlyph[i].height = 0;
            gfxGlyph[i].xOffset = 0;
            gfxGlyph[i].yOffset = 0;
            gfxGlyph[i].xAdvance = psf.width;
        }
        else
        {
            uint8_t startCol = UINT8_MAX, endCol = UINT8_MAX;
        
            for (uint16_t col = 0; col < psf.width; col++)
            {
                if (colMask[col >> 3] & (1 << (7 - (col & 7))))
                {
                    if (startCol == UINT8_MAX)
                    {
                        startCol = col;
                        endCol = col;
                    }
                    else
                    {
                        endCol = col;
                    }
                }
            }
        
            gfxGlyph[i].width = endCol - startCol + 1;
            gfxGlyph[i].height = endRow - startRow + 1;
            gfxGlyph[i].xOffset = startCol;
            gfxGlyph[i].yOffset = startRow;
            gfxGlyph[i].xAdvance = psf.width;
            
            // copy one bit at a time (inefficient but straightforward)
            p = psfOffset + (startRow * psf.widthBytes);
            uint16_t wbit = 7;
            uint8_t bitmapByte = 0;
            
            for (int row = startRow; row <= endRow; row++)
            {
                for (int col = startCol; col <= endCol; col++)
                {
                    if (psf.data[p + (col >> 3)] & (1 << (7 - (col & 7))))
                    {
                        bitmapByte |= (1 << wbit);
                    }
                    
                    if (wbit == 0)
                    {
                        wbit = 7;
                        fprintf(outputFile, " 0x%02x,", bitmapByte);
                        bitmapOffset++;
                        bitmapByte = 0;
                    }
                    else
                    {
                        wbit--;
                    }
                }
                
                p += psf.widthBytes;
            }
            
            if (wbit != 7)
            {
                fprintf(outputFile, " 0x%02x,", bitmapByte);
                bitmapOffset++;
            }
        }
        
        fprintf(outputFile, "\n");
    }

    fprintf(outputFile, "};\n");
    fprintf(outputFile, "\n");
    fprintf(outputFile, "const GFXglyph %sGlyphs[] PROGMEM = {\n", gfxFontName);
    
    for (uint16_t i = 0; i < last - first + 1; i++)
    {
        fprintf(outputFile, "    /* '%c' */ { %u, %u, %u, %u, %d, %d },\n",
            first + i,
            gfxGlyph[i].bitmapOffset, gfxGlyph[i].width, gfxGlyph[i].height,
            gfxGlyph[i].xAdvance, gfxGlyph[i].xOffset, gfxGlyph[i].yOffset);
    }
    
    fprintf(outputFile, "};\n");
    fprintf(outputFile, "\n");
    fprintf(outputFile, "const GFXfont %s PROGMEM = {\n", gfxFontName);

    fprintf(outputFile, "    (uint8_t *)%sBitmaps, (GFXglyph *)%sGlyphs, %u, %u, %u\n",
            gfxFontName, gfxFontName, first, last, psf.height);

    fprintf(outputFile, "};\n");
    
    if (outputFile != stdout)
    {
        fclose(outputFile);
    }
    
    outputFile = NULL;
    return 0;
}
