#include "esp_log.h"
#include "PCFFont.h"
#include "ili9341Driver.h"

typedef uint16_t PCFGlyphIndex;

static const char* TAG = "PCFReader";

int32_t table_count;
struct toc_entry{
    int32_t type;              /* See below, indicates which table */
    int32_t format;            /* See below, indicates how the data are formatted in the table */
    int32_t size;              /* In bytes */
    int32_t offset;            /* from start of file */
} *tables;

uint8_t PCFEncodingTableIndex;
struct{
    uint32_t format;                 /* Always stored with least significant byte first! */
    uint16_t min_char_or_byte2;        /* As in XFontStruct */
    uint16_t max_char_or_byte2;        /* As in XFontStruct */
    uint16_t min_byte1;                /* As in XFontStruct */
    uint16_t max_byte1;                /* As in XFontStruct */
    uint16_t default_char;             /* As in XFontStruct */
    uint16_t glyphindeces;
                                    /* Gives the glyph index that corresponds to each encoding value */
                                    /* a value of 0xffff means no glyph for that encoding */
} *PCFEncodingTable;

typedef struct {
    uint8_t left_sided_bearing;
    uint8_t right_side_bearing;
    uint8_t character_width;
    uint8_t character_ascent;
    uint8_t character_descent;
    /* Implied character attributes field = 0 */
}PCFCompressedMetrics;
typedef struct {
    int16_t left_sided_bearing;
    int16_t right_side_bearing;
    int16_t character_width;
    int16_t character_ascent;
    int16_t character_descent;
    uint16_t character_attributes;
}PCFUncompressedMetrics;

uint8_t PCFMetricsTableIndex;
struct PCFCompressedMetricsTable{
    int32_t format;                 /* Always stored with least significant byte first! */
    int16_t metrics_count;
    PCFCompressedMetrics metrics;
};
struct PCFUncompressedMetricsTable{
    int32_t format;                 /* Always stored with least significant byte first! */
    int32_t metrics_count;
    PCFUncompressedMetrics metrics;
};
void* PCFMetricsTable;

uint8_t PCFBitmapTableIndex;
struct{
    int32_t format;                 /* Always stored with least significant byte first! */
    int32_t glyph_count;            /* byte ordering depends on format, should be the same as the metrics count */
    int32_t offsets;               /* byte offsets to bitmap data */
    int32_t bitmapSizes0;         /* the size the bitmap data will take up depending on various padding options */
    int32_t bitmapSizes1;         /*  which one is actually used in the file is given by (format&3) */
    int32_t bitmapSizes2;
    int32_t bitmapSizes3;
    uint8_t bitmap_data;              /* the bitmap data. format contains flags that indicate: */
                                    /* the byte order (format&4 => LSByte first)*/
                                    /* the bit order (format&8 => LSBit first) */
                                    /* how each row in each glyph's bitmap is padded (format&3) */
                                    /*  0=>bytes, 1=>shorts, 2=>ints */
                                    /* what the bits are stored in (bytes, shorts, ints) (format>>4)&3 */
                                    /*  0=>bytes, 1=>shorts, 2=>ints */
} *PCFBitmapTable;

esp_err_t PCFReaderInit(){
    if (*(int32_t*)pcf_start == 0x01666370){
        return ESP_FAIL;
    }
    table_count = pcf_start[4];
    tables = (struct toc_entry*)(pcf_start+8);
    
    for (uint8_t i = 0; i < table_count; i++){
        switch (tables[i].type){
        case PCF_BDF_ENCODINGS:
            PCFEncodingTableIndex = i;
            PCFEncodingTable = (void*)(pcf_start+tables[i].offset);
            break;
        case PCF_METRICS:
            PCFMetricsTableIndex = i;
            PCFMetricsTable = (void*)(pcf_start+tables[i].offset);
            break;
        case PCF_BITMAPS:
            PCFBitmapTableIndex = i;
            PCFBitmapTable = (void*)(pcf_start+tables[i].offset);
            break;
        default:
            break;
        }
    }
    
    return ESP_OK;
}

PCFGlyphIndex PCFEncode(uint16_t unicode){
    if (unicode < 256){
        return *((&PCFEncodingTable->glyphindeces)+unicode-PCFEncodingTable->min_char_or_byte2);
    }else{
        unicode = ((unicode&0xff00)>>8) | ((unicode&0xff)<<8);
        return ((*((&PCFEncodingTable->glyphindeces)+((unicode&0xff)-(PCFEncodingTable->min_byte1))*((PCFEncodingTable->max_char_or_byte2)-(PCFEncodingTable->min_char_or_byte2)+1)+(unicode>>8)-(PCFEncodingTable->min_char_or_byte2))));
    }
}

uint16_t PCFGetGlyphWidth(PCFGlyphIndex glyph){
    // glyph = ((glyph&0xff00)>>8) | ((glyph&0xff)<<8);
    if (*(uint32_t*)PCFMetricsTable == PCF_DEFAULT_FORMAT){
        // 下面的return不一定可用
        return (&(((struct PCFUncompressedMetricsTable*)PCFMetricsTable)->metrics)+sizeof(PCFUncompressedMetrics)*glyph)->character_width;
    }else{
        return (*((uint8_t*)PCFMetricsTable+6+glyph*5+2))-0x80;
    }
}

esp_err_t PCFPrintStringToILI9341(const ili9341_config_t device, uint16_t x1, uint16_t y1, char* string, size_t len){
    size_t glyphNumber = 0;
    size_t offset;
    
    // 计算 UTF-8 字符数量
    for (offset=0; offset<len;){
        if ((string[offset] & 0b10000000) == 0b10000000){
            if ((string[offset] & 0b11110000) == 0b11110000){
                // U+010000-U+10FFFF
                offset += 4;
            }else if ((string[offset] & 0b11100000) == 0b11100000){
                // U+0800-U+FFFF
                offset += 3;
            }else if ((string[offset] & 0b11000000) == 0b11000000){
                // U+0800-U+FFFF
                offset += 2;
            }
        }else{
            // U+0000-U+007F
            offset++;
        }
        glyphNumber++;
    }

    // 将 UTF-8 的字符进行编码
    PCFGlyphIndex* glyphList = malloc(glyphNumber * sizeof(PCFGlyphIndex));
    offset=0;
    for (size_t i=0; i<glyphNumber; i++){
        if ((string[offset] & 0b10000000) == 0b10000000){
            if ((string[offset] & 0b11110000) == 0b11110000){
                // U+010000-U+10FFFF
                glyphList[i] = PCFEncode(((string[offset]&7)<<18) | ((string[offset+1]&63)<<12) | ((string[offset+2]&63)<<6) | (string[offset+3]&63));
                offset += 4;
            }else if ((string[offset] & 0b11100000) == 0b11100000){
                // U+0800-U+FFFF
                glyphList[i] = PCFEncode(((string[offset]&15)<<12) | ((string[offset+1]&63)<<6) | (string[offset+2]&63));
                offset += 3;
            }else if ((string[offset] & 0b11000000) == 0b11000000){
                // U+0800-U+FFFF
                glyphList[i] = PCFEncode(((string[offset]&31)<<6) | (string[offset+1]&63));
                offset += 2;
            }
        }else{
            // U+0000-U+007F
            glyphList[i] = PCFEncode(string[offset]);
            offset++;
        }
    }
    uint16_t data[512];
    for (size_t i=0; i<glyphNumber; i++){
        size_t data_len = 16*PCFGetGlyphWidth(glyphList[i])*2;
        if (data_len > 512){
            ESP_LOGE(TAG, "data_len %d > 512", data_len);
            return ESP_FAIL;
        }
        ptrdiff_t glyphStart = *((&(PCFBitmapTable->offsets))+glyphList[i]);
        uint8_t* glyph = ((uint8_t*)((&PCFBitmapTable->bitmap_data)+(PCFBitmapTable->glyph_count*4)-4+glyphStart));
        size_t glyphIndex = 0;
        for (size_t i=0; i<data_len; i+=8){
            data[i+7] =  (glyph[glyphIndex]&1)*0xffff;
            data[i+6] = ((glyph[glyphIndex]&2)>>1)*0xffff;
            data[i+5] = ((glyph[glyphIndex]&4)>>2)*0xffff;
            data[i+4] = ((glyph[glyphIndex]&8)>>3)*0xffff;
            data[i+3] = ((glyph[glyphIndex]&16)>>4)*0xffff;
            data[i+2] = ((glyph[glyphIndex]&32)>>5)*0xffff;
            data[i+1] = ((glyph[glyphIndex]&64)>>6)*0xffff;
            data[i+0] = ((glyph[glyphIndex]&128)>>7)*0xffff;
            glyphIndex++;
        }
        ili9341_write(device, x1,y1, x1+PCFGetGlyphWidth(glyphList[i])-1, y1+15, data, data_len);
        x1 += PCFGetGlyphWidth(glyphList[i]);
    }
    free(glyphList);
    return ESP_OK;
}
