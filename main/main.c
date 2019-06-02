#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_flash_data_types.h"
#include "rom/crc.h"

#include <string.h>

#include "odroid_sdcard.h"
#include "odroid_display.h"
#include "input.h"

#include "../components/ugui/ugui.h"

#define USE_PATCHED_ESP_IDF // comment if you don't have esp_partition_reload_table()

#define ESP_PARTITION_TABLE_OFFSET CONFIG_PARTITION_TABLE_OFFSET /* Offset of partition table. Backwards-compatible name.*/
#define ESP_PARTITION_TABLE_MAX_LEN 0xC00 /* Maximum length of partition table data */
#define ESP_PARTITION_TABLE_MAX_ENTRIES (ESP_PARTITION_TABLE_MAX_LEN / sizeof(esp_partition_info_t)) /* Maximum length of partition table data, including terminating entry */

#define PART_SUBTYPE_FACTORY 0x00
#define PART_SUBTYPE_FACTORY_DATA 0xFE

#define PARTS_MAX 20

#define TILE_WIDTH (86)
#define TILE_HEIGHT (48)
#define TILE_LENGTH (TILE_WIDTH * TILE_HEIGHT * 2)

#define APP_MAGIC 0x1205

static RTC_NOINIT_ATTR int set_boot_needed = 0;

const char* SD_CARD = "/sd";
//const char* HEADER = "ODROIDGO_FIRMWARE_V00_00";
const char* HEADER_V00_01 = "ODROIDGO_FIRMWARE_V00_01";

#define FIRMWARE_DESCRIPTION_SIZE (40)
char FirmwareDescription[FIRMWARE_DESCRIPTION_SIZE];

// <partition type=0x00 subtype=0x00 label='name' flags=0x00000000 length=0x00000000>
// 	<data length=0x00000000>
// 		[...]
// 	</data>
// </partition>
typedef struct
{
    uint8_t type;
    uint8_t subtype;
    uint8_t _reserved0;
    uint8_t _reserved1;

    uint8_t label[16];

    uint32_t flags;
    uint32_t length;
} odroid_partition_t;

typedef struct
{
    uint16_t magic;
    uint32_t startOffset;
    uint32_t endOffset;
    char description[FIRMWARE_DESCRIPTION_SIZE];
    odroid_partition_t parts[PARTS_MAX];
    uint8_t parts_count;
    uint8_t tile[TILE_LENGTH];
    uint8_t _reserved[256];
} odroid_app_t;

// ------

static odroid_app_t* apps;
static int apps_count = -1;
static int apps_max = 4;

static esp_partition_info_t* partition_data;
static int partition_count = -1;
static int startTableEntry = -1;
static int startFlashAddress = -1;

uint16_t fb[320 * 240];
UG_GUI gui;
char tempstring[512];

#define ITEM_COUNT (4)
char** files;
int fileCount;
const char* path = "/sd/odroid/firmware";
char* VERSION = NULL;

esp_err_t sdcardret;


static void ui_draw_title(const char*, const char*);


void indicate_error()
{
    int level = 0;
    while (true) {
        gpio_set_level(GPIO_NUM_2, level);
        level = !level;
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

static void pset(UG_S16 x, UG_S16 y, UG_COLOR color)
{
    fb[y * 320 + x] = color;
}

static void ui_update_display()
{
    ili9341_write_frame_rectangleLE(0, 0, 320, 240, fb);
}

static void ui_draw_image(short x, short y, short width, short height, uint16_t* data)
{
    for (short i = 0 ; i < height; ++i)
    {
        for (short j = 0; j < width; ++j)
        {
            uint16_t pixel = data[i * width + j];
            UG_DrawPixel(x + j, y + i, pixel);
        }
    }
}

// TODO: default bad image tile
void ui_firmware_image_get(const char* filename, uint16_t* outData)
{
    //printf("%s: filename='%s'\n", __func__, filename);
    const uint8_t DEFAULT_DATA = 0xff;

    FILE* file = fopen(filename, "rb");
    if (!file)
    {
        memset(outData, DEFAULT_DATA, TILE_LENGTH);
        return;
    }

    // Check the header
    const size_t headerLength = strlen(HEADER_V00_01);
    char* header = malloc(headerLength + 1);
    if(!header)
    {
        memset(outData, DEFAULT_DATA, TILE_LENGTH);
        goto ui_firmware_image_get_exit;
    }

    // null terminate
    memset(header, 0, headerLength + 1);

    size_t count = fread(header, 1, headerLength, file);
    if (count != headerLength)
    {
        memset(outData, DEFAULT_DATA, TILE_LENGTH);
        goto ui_firmware_image_get_exit;
    }

    if (strncmp(HEADER_V00_01, header, headerLength) != 0)
    {
        memset(outData, DEFAULT_DATA, TILE_LENGTH);
        goto ui_firmware_image_get_exit;
    }

    //printf("Header OK: '%s'\n", header);

    // read description
    count = fread(FirmwareDescription, 1, FIRMWARE_DESCRIPTION_SIZE, file);
    if (count != FIRMWARE_DESCRIPTION_SIZE)
    {
        memset(outData, DEFAULT_DATA, TILE_LENGTH);
        goto ui_firmware_image_get_exit;
    }

    // read tile
    count = fread(outData, 1, TILE_LENGTH, file);
    if (count != TILE_LENGTH)
    {
        memset(outData, DEFAULT_DATA, TILE_LENGTH);
    }


ui_firmware_image_get_exit:
    free(header);
    fclose(file);
}

static void ClearScreen()
{
}

static void UpdateDisplay()
{
    ui_update_display();
}

static void DisplayError(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = (240 / 2) - (12 / 2);
    UG_SetForecolor(C_RED);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

static void DisplayMessage(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = (240 / 2) + 8 + (12 / 2) + 16;
    UG_SetForecolor(C_BLACK);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

static void DisplayProgress(int percent)
{
    if (percent > 100) percent = 100;

    const int WIDTH = 200;
    const int HEIGHT = 12;
    const int FILL_WIDTH = WIDTH * (percent / 100.0f);

    short left = (320 / 2) - (WIDTH / 2);
    short top = (240 / 2) - (HEIGHT / 2) + 16;
    UG_FillFrame(left - 1, top - 1, left + WIDTH + 1, top + HEIGHT + 1, C_WHITE);
    UG_DrawFrame(left - 1, top - 1, left + WIDTH + 1, top + HEIGHT + 1, C_BLACK);

    if (FILL_WIDTH > 0)
    {
        UG_FillFrame(left, top, left + FILL_WIDTH, top + HEIGHT, C_GREEN);
    }

    //UpdateDisplay();
}

static void DisplayFooter(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = 240 - (16 * 2) - 8;
    UG_SetForecolor(C_BLACK);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

static void DisplayHeader(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = (16 + 8);
    UG_SetForecolor(C_BLACK);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}


//---------------
void boot_application()
{
    printf("Booting application.\n");

    // Set firmware active
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (partition == NULL)
    {
        DisplayError("NO BOOT PART ERROR");
        indicate_error();
    }

    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK)
    {
        DisplayError("BOOT SET ERROR");
        indicate_error();
    }
    
    // clear framebuffer
    ili9341_clear(0x0000);

    backlight_deinit();
    
    // reboot
    esp_restart();
}


static void read_app_table()
{
    esp_err_t err;
    
    apps_count = 0;
    
    esp_partition_t *app_table_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE_FACTORY_DATA, NULL);

    startFlashAddress = app_table_part->address + app_table_part->size;

    if (!app_table_part) {
        abort();
    }
    
    apps_max = (app_table_part->size / sizeof(odroid_app_t));
    
    printf("Max apps: %d\n", apps_max);
    if (!apps) {
        apps = malloc(app_table_part->size);
    }

    if (!apps) {
        DisplayError("APP TABLE ALLOC ERROR");
        indicate_error();
    }

    err = esp_partition_read(app_table_part, 0, (void*)apps, app_table_part->size);
    if (err != ESP_OK)
    {
        DisplayError("APP TABLE READ ERROR");
        indicate_error();
    }
    
    for (int i = 0; i < apps_max; i++) {
        if (apps[i].magic != APP_MAGIC) {
            break;
        }
        if (apps[i].endOffset + 1 > startFlashAddress) {
            startFlashAddress = apps[i].endOffset + 1;
        }
        apps_count++;
    }

    //65K align the address, the flashing code below complains otherwise. will investigate later.
    if ((startFlashAddress & 0xffff) != 0) {
        startFlashAddress = (startFlashAddress & 0xffff0000) + 0xffff + 1;
    }
    
    printf("App count: %d\n", apps_count);
}


static void write_app_table()
{
    esp_err_t err;

    esp_partition_t *app_table_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE_FACTORY_DATA, NULL);

    if (!app_table_part || !apps) {
        read_app_table();
    }
    
    for (int i = apps_count; i < apps_max; ++i)
    {
        memset(&apps[i], 0xff, sizeof(odroid_app_t));
    }

    err = esp_partition_erase_range(app_table_part, 0, app_table_part->size);
    if (err != ESP_OK)
    {
        DisplayError("APP TABLE ERASE ERROR");
        indicate_error();
    }

    err = esp_partition_write(app_table_part, 0, (void*)apps, app_table_part->size);
    if (err != ESP_OK)
    {
        DisplayError("APP TABLE WRITE ERROR");
        indicate_error();
    }

    printf("Written app table %d\n", apps_count);
}


static void remove_app(uint8_t index)
{
    //memset(&apps[index], 0xFF, sizeof(odroid_app_t));
    
    if (index == apps_count -1) { // It's the last one, easy then!
        apps_count--;
    }
    else { // We must defrag
        odroid_app_t *app = &apps[index];
        size_t newFlashOffset = app->startOffset;
        size_t deletedappsize = app->endOffset - app->startOffset + 1;
        size_t flashEnd = apps[apps_count - 1].endOffset + 1;

        printf("delete size: %d\n", deletedappsize);

        // Remove item
        for (int i = index + 1; i < apps_count; i++) {
            memcpy(&apps[i - 1], &apps[i], sizeof(odroid_app_t));
        }
        apps_count--;
        
        // Adjust offsets for other apps
        for (int i = index; i < apps_count; i++) {
            apps[i].startOffset -= deletedappsize;
            apps[i].endOffset -= deletedappsize;
        }
        
        // Defrag flash to match the new offsets
        uint8_t *buffer = malloc(4096);

        for (size_t i = newFlashOffset; i < flashEnd; i += 4096) {
            printf("Moving %x to %x\n", i + deletedappsize, i);
            spi_flash_read(i + deletedappsize, buffer, 4096);
            spi_flash_erase_range(i, 4096);
            spi_flash_write(i, buffer, 4096);
            DisplayProgress((float) (i - newFlashOffset) / (float)(flashEnd - newFlashOffset)  * 100.0);
            UpdateDisplay();
        }

        free(buffer);
    }

    if (apps_count > 0) {
        startFlashAddress = apps[apps_count-1].endOffset + 1;
    }

    write_app_table();
}



static void read_partition_table()
{
    esp_err_t err;

    partition_count = 0;
    
    if (!partition_data)
    {
        partition_data = (const esp_partition_info_t*)malloc(ESP_PARTITION_TABLE_MAX_LEN);
    }

    // Read table
    err = spi_flash_read(ESP_PARTITION_TABLE_OFFSET, (void*)partition_data, ESP_PARTITION_TABLE_MAX_LEN);
    if (err != ESP_OK)
    {
        DisplayError("TABLE READ ERROR");
        indicate_error();
    }

    // Find end of first partitioned

    for (int i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; ++i)
    {
        const esp_partition_info_t *part = &partition_data[i];
        if (part->magic == 0xffff) break;

        if (part->magic == ESP_PARTITION_MAGIC)
        {
            partition_count++;

            if (part->type == PART_TYPE_DATA &&
                part->subtype == PART_SUBTYPE_FACTORY_DATA)
            {
                startTableEntry = i + 1;
                break;
            }
        }
    }
}


static void write_partition_table(odroid_partition_t* parts, size_t parts_count, size_t flashOffset)
{
    esp_err_t err;

    if (!partition_data) {
        read_partition_table();
    }

    // Find end of first partitioned
    if (startTableEntry < 0)
    {
        DisplayError("NO FACTORY PARTITION ERROR");
        indicate_error();
    }

    printf("%s: startTableEntry=%d, startFlashAddress=%#08x\n",
        __func__, startTableEntry, flashOffset);

    // blank partition table entries
    for (int i = startTableEntry; i < ESP_PARTITION_TABLE_MAX_ENTRIES; ++i)
    {
        memset(&partition_data[i], 0xff, sizeof(esp_partition_info_t));
    }

    // Add partitions
    size_t offset = 0;
    for (int i = 0; i < parts_count; ++i)
    {
        esp_partition_info_t* part = &partition_data[startTableEntry + i];
        part->magic = ESP_PARTITION_MAGIC;
        part->type = parts[i].type;
        part->subtype = parts[i].subtype;
        part->pos.offset = flashOffset + offset;
        part->pos.size = parts[i].length;
        for (int j = 0; j < 16; ++j)
        {
            part->label[j] = parts[i].label[j];
        }
        part->flags = parts[i].flags;

        offset += parts[i].length;
    }

    //abort();

    // Erase partition table
    if (ESP_PARTITION_TABLE_MAX_LEN > 4096)
    {
        DisplayError("TABLE SIZE ERROR");
        indicate_error();
    }

    err = spi_flash_erase_range(ESP_PARTITION_TABLE_OFFSET, 4096);
    if (err != ESP_OK)
    {
        DisplayError("TABLE ERASE ERROR");
        indicate_error();
    }

    // Write new table
    err = spi_flash_write(ESP_PARTITION_TABLE_OFFSET, (void*)partition_data, ESP_PARTITION_TABLE_MAX_LEN);
    if (err != ESP_OK)
    {
        DisplayError("TABLE WRITE ERROR");
        indicate_error();
    }

#ifdef USE_PATCHED_ESP_IDF
    esp_partition_reload_table();
#endif
}



//uint8_t tileData[TILE_LENGTH];

void flash_firmware(const char* fullPath)
{
    size_t count;

    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    sprintf(&tempstring, "Size: N/A   Destination: 0x%x", startFlashAddress);

    ui_draw_title("Install Application", tempstring);
    UpdateDisplay();

    read_partition_table();
    read_app_table();

    printf("Opening file '%s'.\n", fullPath);

    FILE* file = fopen(fullPath, "rb");
    if (file == NULL)
    {
        DisplayError("FILE OPEN ERROR");
        indicate_error();
    }

    // Check the header
    const size_t headerLength = strlen(HEADER_V00_01);
    char* header = malloc(headerLength + 1);
    if(!header)
    {
        DisplayError("MEMORY ERROR");
        indicate_error();
    }

    // null terminate
    memset(header, 0, headerLength + 1);

    count = fread(header, 1, headerLength, file);
    if (count != headerLength)
    {
        DisplayError("HEADER READ ERROR");
        indicate_error();
    }

    if (strncmp(HEADER_V00_01, header, headerLength) != 0)
    {
        DisplayError("HEADER MATCH ERROR");
        indicate_error();
    }

    printf("Header OK: '%s'\n", header);
    free(header);

    // read description
    count = fread(FirmwareDescription, 1, FIRMWARE_DESCRIPTION_SIZE, file);
    if (count != FIRMWARE_DESCRIPTION_SIZE)
    {
        DisplayError("DESCRIPTION READ ERROR");
        indicate_error();
    }

    // ensure null terminated
    FirmwareDescription[FIRMWARE_DESCRIPTION_SIZE - 1] = 0;

    printf("FirmwareDescription='%s'\n", FirmwareDescription);
    DisplayHeader(FirmwareDescription);
    //UpdateDisplay();

    // Tile
    uint16_t* tileData = malloc(TILE_LENGTH);
    if (!tileData)
    {
        DisplayError("TILE MEMORY ERROR");
        indicate_error();
    }

    count = fread(tileData, 1, TILE_LENGTH, file);
    if (count != TILE_LENGTH)
    {
        DisplayError("TILE READ ERROR");
        indicate_error();
    }

    const uint16_t tileLeft = (320 / 2) - (TILE_WIDTH / 2);
    const uint16_t tileTop = (16 + 16 + 16);
    ui_draw_image(tileLeft, tileTop,
        TILE_WIDTH, TILE_HEIGHT, tileData);

    //free(tileData);

    // Tile border
    UG_DrawFrame(tileLeft - 1, tileTop - 1, tileLeft + TILE_WIDTH, tileTop + TILE_HEIGHT, C_BLACK);
    UpdateDisplay();

    // start to begin, b back
    DisplayMessage("[START]");
    DisplayFooter("[B] Cancel");
    //UpdateDisplay();
    
    while (1) {
        int btn = wait_for_button_press(-1);

        if (btn == ODROID_INPUT_START) break;
        if (btn == ODROID_INPUT_B)
        {
            fclose(file);
            return;
        }
    }

    DisplayMessage("");
    DisplayFooter("");
    //UpdateDisplay();


    DisplayMessage("Verifying ...");


    const int ERASE_BLOCK_SIZE = 4096;
    void* data = malloc(ERASE_BLOCK_SIZE);
    if (!data)
    {
        DisplayError("DATA MEMORY ERROR");
        indicate_error();
    }


    // Verify file integerity
    size_t current_position = ftell(file);


    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);


    uint32_t expected_checksum;
    fseek(file, file_size - sizeof(expected_checksum), SEEK_SET);
    count = fread(&expected_checksum, 1, sizeof(expected_checksum), file);
    if (count != sizeof(expected_checksum))
    {
        DisplayError("CHECKSUM READ ERROR");
        indicate_error();
    }
    printf("%s: expected_checksum=%#010x\n", __func__, expected_checksum);


    fseek(file, 0, SEEK_SET);

    uint32_t checksum = 0;
    size_t check_offset = 0;
    while(true)
    {
        count = fread(data, 1, ERASE_BLOCK_SIZE, file);
        if (check_offset + count == file_size)
        {
            count -= 4;
        }

        checksum = crc32_le(checksum, data, count);
        check_offset += count;

        if (count < ERASE_BLOCK_SIZE) break;
    }

    printf("%s: checksum=%#010x\n", __func__, checksum);

    if (checksum != expected_checksum)
    {
        DisplayError("CHECKSUM MISMATCH ERROR");
        indicate_error();
    }

    // restore location to end of description
    fseek(file, current_position, SEEK_SET);

    //while(1) vTaskDelay(1);

    int parts_count = 0;
    odroid_partition_t* parts = malloc(sizeof(odroid_partition_t) * PARTS_MAX);
    if (!parts)
    {
        DisplayError("PARTITION MEMORY ERROR");
        indicate_error();
    }

    // Copy the firmware
    size_t curren_flash_address = startFlashAddress;

    while(true)
    {
        if (ftell(file) >= (file_size - sizeof(checksum)))
        {
            break;
        }

        // Partition
        odroid_partition_t slot;
        count = fread(&slot, 1, sizeof(slot), file);
        if (count != sizeof(slot))
        {
            DisplayError("PARTITION READ ERROR");
            indicate_error();
        }

        if (parts_count >= PARTS_MAX)
        {
            DisplayError("PARTITION COUNT ERROR");
            indicate_error();
        }

        if (slot.type == 0xff)
        {
            DisplayError("PARTITION TYPE ERROR");
            indicate_error();
        }

        if (curren_flash_address + slot.length > 16 * 1024 * 1024)
        {
            DisplayError("PARTITION LENGTH ERROR");
            indicate_error();
        }

        if ((curren_flash_address & 0xffff0000) != curren_flash_address)
        {
            DisplayError("PARTITION LENGTH ALIGNMENT ERROR");
            indicate_error();
        }


        // Data Length
        uint32_t length;
        count = fread(&length, 1, sizeof(length), file);
        if (count != sizeof(length))
        {
            DisplayError("LENGTH READ ERROR");
            indicate_error();
        }

        if (length > slot.length)
        {
            printf("%s: data length error - length=%x, slot.length=%x\n",
                __func__, length, slot.length);

            DisplayError("DATA LENGTH ERROR");
            indicate_error();
        }

        size_t nextEntry = ftell(file) + length;

        if (length > 0)
        {
            // turn LED off
            gpio_set_level(GPIO_NUM_2, 0);


            // erase
            int eraseBlocks = length / ERASE_BLOCK_SIZE;
            if (eraseBlocks * ERASE_BLOCK_SIZE < length) ++eraseBlocks;

            // Display
            sprintf(tempstring, "Erasing ... (%d)", parts_count);

            printf("%s\n", tempstring);
            DisplayProgress(0);
            DisplayMessage(tempstring);

            esp_err_t ret = spi_flash_erase_range(curren_flash_address, eraseBlocks * ERASE_BLOCK_SIZE);
            if (ret != ESP_OK)
            {
                printf("spi_flash_erase_range failed. eraseBlocks=%d\n", eraseBlocks);
                DisplayError("ERASE ERROR");
                indicate_error();
            }


            // turn LED on
            gpio_set_level(GPIO_NUM_2, 1);


            // Write data
            int totalCount = 0;
            for (int offset = 0; offset < length; offset += ERASE_BLOCK_SIZE)
            {
                // Display
                sprintf(tempstring, "Writing (%d)", parts_count);

                printf("%s - %#08x\n", tempstring, offset);
                DisplayProgress((float)offset / (float)(length - ERASE_BLOCK_SIZE) * 100.0f);
                DisplayMessage(tempstring);

                // read
                //printf("Reading offset=0x%x\n", offset);
                count = fread(data, 1, ERASE_BLOCK_SIZE, file);
                if (count <= 0)
                {
                    DisplayError("DATA READ ERROR");
                    indicate_error();
                }

                if (offset + count >= length)
                {
                    count = length - offset;
                }


                // flash
                //printf("Writing offset=0x%x\n", offset);
                //ret = esp_partition_write(part, offset, data, count);
                ret = spi_flash_write(curren_flash_address + offset, data, count);
                if (ret != ESP_OK)
        		{
        			printf("spi_flash_write failed. address=%#08x\n", curren_flash_address + offset);
                    DisplayError("WRITE ERROR");
                    indicate_error();
        		}

                totalCount += count;
            }

            if (totalCount != length)
            {
                printf("Size mismatch: lenght=%#08x, totalCount=%#08x\n", length, totalCount);
                DisplayError("DATA SIZE ERROR");
                indicate_error();
            }


            // TODO: verify




            // Notify OK
            sprintf(tempstring, "OK: [%d] Length=%#08x", parts_count, length);

            printf("%s\n", tempstring);
            //DisplayFooter(tempstring);
        }

        parts[parts_count++] = slot;
        curren_flash_address += slot.length;


        // Seek to next entry
        if (fseek(file, nextEntry, SEEK_SET) != 0)
        {
            DisplayError("SEEK ERROR");
            indicate_error();
        }

    }

    close(file);


    // Utility
    FILE* util = fopen("/sd/odroid/firmware/utility.bin", "rb");
    if (util)
    {
        if ((curren_flash_address & 0xffff0000) != curren_flash_address)
        {
            DisplayError("ALIGNMENT ERROR");
            indicate_error();
        }


        // Get file size
        fseek(util, 0, SEEK_END);
        size_t length = ftell(util);
        fseek(util, 0, SEEK_SET);

        printf("utility.bin - length=%d\n", length);


        // TODO: Determine if there is room


        // turn LED off
        gpio_set_level(GPIO_NUM_2, 0);


        // Erase
        int eraseBlocks = length / ERASE_BLOCK_SIZE;
        if (eraseBlocks * ERASE_BLOCK_SIZE < length) ++eraseBlocks;

        // Display
        sprintf(tempstring, "Erasing Utility ...");

        printf("%s\n", tempstring);
        DisplayProgress(0);
        DisplayMessage(tempstring);

        esp_err_t ret = spi_flash_erase_range(curren_flash_address, eraseBlocks * ERASE_BLOCK_SIZE);
        if (ret != ESP_OK)
        {
            printf("spi_flash_erase_range failed. eraseBlocks=%d\n", eraseBlocks);
            DisplayError("ERASE ERROR");
            indicate_error();
        }


        // turn LED on
        gpio_set_level(GPIO_NUM_2, 1);


        // Write data
        int totalCount = 0;
        for (int offset = 0; offset < length; offset += ERASE_BLOCK_SIZE)
        {
            // Display
            sprintf(tempstring, "Writing Utility");

            printf("%s - %#08x\n", tempstring, offset);
            DisplayProgress((float)offset / (float)(length - ERASE_BLOCK_SIZE) * 100.0f);
            DisplayMessage(tempstring);

            // read
            //printf("Reading offset=0x%x\n", offset);
            count = fread(data, 1, ERASE_BLOCK_SIZE, util);
            if (count <= 0)
            {
                DisplayError("DATA READ ERROR");
                indicate_error();
            }

            if (offset + count >= length)
            {
                count = length - offset;
            }


            // flash
            //printf("Writing offset=0x%x\n", offset);
            //ret = esp_partition_write(part, offset, data, count);
            ret = spi_flash_write(curren_flash_address + offset, data, count);
            if (ret != ESP_OK)
            {
                printf("spi_flash_write failed. address=%#08x\n", curren_flash_address + offset);
                DisplayError("WRITE ERROR");
                indicate_error();
            }

            totalCount += count;
        }

        // Add partition
        odroid_partition_t util_part;
        memset(&util_part, 0, sizeof(util_part));


        util_part.type = ESP_PARTITION_TYPE_APP;
        util_part.subtype = ESP_PARTITION_SUBTYPE_APP_TEST;

        strcpy((char*)util_part.label, "utility");

        util_part.flags = 0;

        // 64k align
        if ((length & 0xffff0000) != length) length += 0x10000;
        util_part.length = length & 0xffff0000;


        parts[parts_count++] = util_part;

        fclose(util);
    }


    // Write partition table
    write_partition_table(parts, parts_count, startFlashAddress);

    // Write app table
    odroid_app_t *app = &apps[apps_count++];
    app->magic = APP_MAGIC;
    app->startOffset = startFlashAddress;
    app->endOffset = curren_flash_address - 1;
    app->parts_count = parts_count;
    memcpy(&app->description, FirmwareDescription, FIRMWARE_DESCRIPTION_SIZE);
    memcpy(&app->parts, parts, sizeof(odroid_partition_t) * PARTS_MAX);
    memcpy(&app->tile, tileData, TILE_LENGTH);
    write_app_table();

    free(tileData);
    free(data);

    // turn LED off
    gpio_set_level(GPIO_NUM_2, 0);

    DisplayMessage("Ready !");
    DisplayFooter("[B] Go Back   |   [A] Boot");
    
    while (1) {
        int btn = wait_for_button_press(-1);

        if (btn == ODROID_INPUT_A) break;
        if (btn == ODROID_INPUT_B) return;
    }
    
    // Close SD card
    odroid_sdcard_close();

    // clear framebuffer
    ili9341_clear(0x0000);

    // boot firmware
#ifdef USE_PATCHED_ESP_IDF
    boot_application();
#endif
    set_boot_needed = 1;
    esp_restart();
    
    indicate_error();
}


static void ui_draw_title(const char* TITLE, const char* FOOTER)
{
    UG_FillFrame(0, 0, 319, 239, C_WHITE);

    // Header
    UG_FillFrame(0, 0, 319, 15, C_MIDNIGHT_BLUE);
    UG_FontSelect(&FONT_8X8);
    const short titleLeft = (320 / 2) - (strlen(TITLE) * 9 / 2);
    UG_SetForecolor(C_WHITE);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);
    UG_PutString(titleLeft, 4, TITLE);

    // Footer
    UG_FontSelect(&FONT_8X8);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);
    UG_SetForecolor(C_LIGHT_GRAY);
    UG_FillFrame(0, 239 - 16, 319, 239, C_MIDNIGHT_BLUE);
    const short footerLeft = (320 / 2) - (strlen(FOOTER) * 9 / 2);
    UG_PutString(footerLeft, 240 - 4 - 8, FOOTER);
}


static void ui_draw_page(char** files, int fileCount, int currentItem)
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    int page = currentItem / ITEM_COUNT;
    page *= ITEM_COUNT;

    sprintf(&tempstring, "Free space: %.2fMB", (double)(0x1000000 - startFlashAddress) / 1024 / 1024);
    
    ui_draw_title("Select a file", tempstring);

    const int innerHeight = 240 - (16 * 2); // 208
    const int itemHeight = innerHeight / ITEM_COUNT; // 52

    const int rightWidth = (213); // 320 * (2.0 / 3.0)
    const int leftWidth = 320 - rightWidth;

    // Tile width = 86, height = 48 (16:9)
    const short imageLeft = (leftWidth / 2) - (86 / 2);
    const short textLeft = 320 - rightWidth;


	if (fileCount < 1)
	{   
        DisplayMessage("SD Card Empty");
        UpdateDisplay();
	}
	else
	{
        uint16_t* tile = malloc(TILE_LENGTH);
        if (!tile) abort();

        char* displayStrings[ITEM_COUNT];
        for(int i = 0; i < ITEM_COUNT; ++i)
        {
            displayStrings[i] = NULL;
        }

	    for (int line = 0; line < ITEM_COUNT; ++line)
	    {
			if (page + line >= fileCount) break;

            //uint16_t id = TXB_ID_0 + line;
            short top = 16 + (line * itemHeight) - 1;

	        if ((page) + line == currentItem)
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_YELLOW);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_YELLOW);
	        }
	        else
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_WHITE);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_WHITE);
	        }

			char* fileName = files[page + line];
			if (!fileName) abort();

			displayStrings[line] = (char*)malloc(strlen(fileName) + 1);
            strcpy(displayStrings[line], fileName);
            displayStrings[line][strlen(fileName) - 3] = 0; // ".fw" = 3


            size_t fullPathLength = strlen(path) + 1 + strlen(fileName) + 1;
            char* fullPath = (char*)malloc(fullPathLength);
            if (!fullPath) abort();

            strcpy(fullPath, path);
            strcat(fullPath, "/");
            strcat(fullPath, fileName);
            ui_firmware_image_get(fullPath, tile);
            ui_draw_image(imageLeft, top + 2, TILE_WIDTH, TILE_HEIGHT, tile);

            free(fullPath);

            // Tile border
            //UG_DrawFrame(imageLeft - 1, top + 1, imageLeft + TILE_WIDTH, top + 2 + TILE_HEIGHT, C_BLACK);

	        //UG_TextboxSetText(&window1, id, displayStrings[line]);
            UG_FontSelect(&FONT_8X12);
            UG_PutString(textLeft, top + 2 + 2 + 16, displayStrings[line]);
	    }

        UpdateDisplay();

        for(int i = 0; i < ITEM_COUNT; ++i)
        {
            free(displayStrings[i]);
        }

        free(tile);
	}
}

const char* ui_choose_file(const char* path)
{
    const char* result = NULL;

    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    files = 0;
    fileCount = odroid_sdcard_files_get(path, ".fw", &files);
    printf("%s: fileCount=%d\n", __func__, fileCount);
    
    // Check SD card
    if (sdcardret != ESP_OK)
    {
        DisplayError("SD CARD ERROR");
        indicate_error();
    }

    // At least one firmware must be available
    if (fileCount < 1)
    {
        DisplayError("NO FILES ERROR");
        indicate_error();
    }


    // Selection
    int currentItem = 0;
    ui_draw_page(files, fileCount, currentItem);

    while (true)
    {
        int page = currentItem / ITEM_COUNT;
        page *= ITEM_COUNT;

        int btn = wait_for_button_press(-1);

        if(btn == ODROID_INPUT_DOWN)
        {
            if (currentItem + 1 < fileCount)
            {
                ++currentItem;
            }
            else
            {
                currentItem = 0;
            }
            ui_draw_page(files, fileCount, currentItem);
        }
        else if(btn == ODROID_INPUT_UP)
        {
            if (currentItem > 0)
            {
                --currentItem;
            }
            else
            {
                currentItem = fileCount - 1;
            }
            ui_draw_page(files, fileCount, currentItem);
        }
        else if(btn == ODROID_INPUT_RIGHT)
        {
            if (page + ITEM_COUNT < fileCount)
            {
                currentItem = page + ITEM_COUNT;
            }
            else
            {
                currentItem = 0;
            }
            ui_draw_page(files, fileCount, currentItem);
        }
        else if(btn == ODROID_INPUT_LEFT)
        {
            if (page - ITEM_COUNT >= 0)
            {
                currentItem = page - ITEM_COUNT;
            }
            else
            {
                currentItem = page;
                while (currentItem + ITEM_COUNT < fileCount)
                {
                    currentItem += ITEM_COUNT;
                }

            }
            ui_draw_page(files, fileCount, currentItem);
        }
        else if(btn == ODROID_INPUT_A)
        {
            size_t fullPathLength = strlen(path) + 1 + strlen(files[currentItem]) + 1;

            char* fullPath = (char*)malloc(fullPathLength);
            if (!fullPath) abort();

            strcpy(fullPath, path);
            strcat(fullPath, "/");
            strcat(fullPath, files[currentItem]);

            result = fullPath;
            break;
        }
        else if (btn == ODROID_INPUT_MENU)
        {
            ui_draw_title("ODROID-GO", VERSION);
            DisplayMessage("Exiting ...");
            UpdateDisplay();

            boot_application();

            // should not reach
            abort();
        }
        else if (btn == ODROID_INPUT_B)
        {
            break;
        }
    }

    odroid_sdcard_files_free(files, fileCount);

    return result;
}



static void ui_draw_dialog(char options[], int optionCount, int currentItem)
{
    int border = 3;
    int itemWidth = 190;
    int itemHeight = 20;
    int width = itemWidth + (border * 2);
    int height = ((optionCount+1) * itemHeight) + (border *  2);
    int top = (240 - height) / 2;
    int left  = (320 - width) / 2;

    UG_FillFrame(left, top, left + width, top + height, C_BLUE);
    UG_FillFrame(left + border, top + border, left + width - border, top + height - border, C_WHITE);
    
    top += border;
    left += border;
    
    for (int i = 0; i < optionCount; i++) {
        int fg = (i == currentItem) ? C_WHITE : C_BLACK;
        int bg = (i == currentItem) ? C_BLUE : C_WHITE;

        UG_SetForecolor(fg);
        UG_SetBackcolor(bg);
        UG_FillFrame(left, top, left + itemWidth, top + itemHeight, bg);
        UG_FontSelect(&FONT_8X12);
        UG_PutString(left + 2, top + 2, &options[i * 32]);

        top += itemHeight;
    }

    // Display version at the bottom
    UG_SetForecolor(C_GRAY);
    UG_SetBackcolor(C_WHITE);
    UG_FontSelect(&FONT_8X8);
    UG_PutString(left + 2, top + 2, "Version:\n " COMPILEDATE "-" GITREV);

    UpdateDisplay();
}


static int ui_choose_dialog(char options[], int optionCount, bool cancellable)
{
    int currentItem = 0;
    ui_draw_dialog(options, optionCount, currentItem);

    while (true)
    {
		int btn = wait_for_button_press(-1);

        if(btn == ODROID_INPUT_DOWN)
        {
            if (currentItem + 1 < optionCount) ++currentItem;
            else currentItem = 0;

            ui_draw_dialog(options, optionCount, currentItem);
        }
        else if(btn == ODROID_INPUT_UP)
        {
            if (currentItem > 0) --currentItem;
            else currentItem = optionCount - 1;

            ui_draw_dialog(options, optionCount, currentItem);
        }
        else if(btn == ODROID_INPUT_A)
        {
            return currentItem;
        }
        else if(btn == ODROID_INPUT_B)
        {
            if (cancellable) {
                return -1;
            }
        }
    }
    
    return currentItem;
}


static void ui_draw_app_page(int currentItem)
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    int page = currentItem / ITEM_COUNT;
    page *= ITEM_COUNT;

    ui_draw_title("ODROID-GO", "[START] Menu    |    [A] Boot App");

    const int innerHeight = 240 - (16 * 2); // 208
    const int itemHeight = innerHeight / ITEM_COUNT; // 52

    const int rightWidth = (213); // 320 * (2.0 / 3.0)
    const int leftWidth = 320 - rightWidth;

    // Tile width = 86, height = 48 (16:9)
    const short imageLeft = (leftWidth / 2) - (86 / 2);
    const short textLeft = 320 - rightWidth;

	if (apps_count < 1)
	{
        DisplayMessage("No apps have been flashed yet!");
	}
	else
	{
	    for (int line = 0; line < ITEM_COUNT; ++line)
	    {
            
			if (page + line >= apps_count) break;

            odroid_app_t *app = &apps[page + line];
            
            short top = 16 + (line * itemHeight) - 1;

	        if ((page) + line == currentItem)
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_YELLOW);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_YELLOW);
	        }
	        else
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_WHITE);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_WHITE);
	        }

            ui_draw_image(imageLeft, top + 2, TILE_WIDTH, TILE_HEIGHT, app->tile);

            UG_FontSelect(&FONT_8X12);
            UG_PutString(textLeft, top + 2 + 2 + 6, app->description);
            sprintf(&tempstring, "0x%x - 0x%x", app->startOffset, app->endOffset);
            UG_PutString(textLeft, top + 2 + 2 + 24, tempstring);
	    }
	}

    UpdateDisplay();
}


void ui_choose_app()
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    // Selection
    int currentItem = 0;
    ui_draw_app_page(currentItem);

    while (true)
    {
        int btn = wait_for_button_press(100);

        int page = currentItem / ITEM_COUNT;
        page *= ITEM_COUNT;

		if (apps_count > 0)
		{
	        if(btn == ODROID_INPUT_DOWN)
	        {
                if (currentItem + 1 < apps_count)
                {
                    ++currentItem;
                }
                else
                {
                    currentItem = 0;
                }
                ui_draw_app_page(currentItem);
	        }
	        else if(btn == ODROID_INPUT_UP)
	        {
                if (currentItem > 0)
                {
                    --currentItem;
                }
                else
                {
                    currentItem = apps_count - 1;
                }
                ui_draw_app_page(currentItem);
	        }
	        else if(btn == ODROID_INPUT_RIGHT)
	        {
                if (page + ITEM_COUNT < apps_count)
                {
                    currentItem = page + ITEM_COUNT;
                }
                else
                {
                    currentItem = 0;
                }
                ui_draw_app_page(currentItem);
	        }
	        else if(btn == ODROID_INPUT_LEFT)
	        {
                if (page - ITEM_COUNT >= 0)
                {
                    currentItem = page - ITEM_COUNT;
                }
                else
                {
                    currentItem = page;
                    while (currentItem + ITEM_COUNT < apps_count)
                    {
                        currentItem += ITEM_COUNT;
                    }
                }
                ui_draw_app_page(currentItem);
	        }
	        else if(btn == ODROID_INPUT_A)
	        {
                odroid_app_t *app = &apps[currentItem];
                write_partition_table(app->parts, app->parts_count, app->startOffset);
                #ifdef USE_PATCHED_ESP_IDF
                    boot_application();
                #endif
                set_boot_needed = 1;
                esp_restart();
                break;
	        }
        }

        if (btn == ODROID_INPUT_START)
        {
            const char options[5][32] = {
                "Install from SD Card", 
                "Erase selected app",
                "Erase all apps",
                "Erase NVM",
                "Restart System"
            };
            
            int choice = ui_choose_dialog(options, 5, true);
            char* fileName;

            switch(choice) {
                case 0:
                    fileName = ui_choose_file(path);
                    if (fileName) {
                        printf("%s: fileName='%s'\n", __func__, fileName);
                        flash_firmware(fileName);
                        free(fileName);
                    }
                    break;
                case 1:
                    remove_app(currentItem);
                    break;
                case 2:
                    memset(apps, 0xFF, apps_max * sizeof(odroid_app_t));
                    write_app_table();
                    read_app_table();
                    break;
                case 3:
                    nvs_flash_erase();
                    break;
            }

            ui_draw_app_page(currentItem);
        }
    }
}


void app_main(void)
{
    #ifndef USE_PATCHED_ESP_IDF
    if (set_boot_needed == 1) {
        set_boot_needed = 0;
        boot_application();
    }
    #endif

    const char* VER_PREFIX = "Ver: ";
    size_t ver_size = strlen(VER_PREFIX) + strlen(COMPILEDATE) + 1 + strlen(GITREV) + 1;
    VERSION = malloc(ver_size);
    if (!VERSION) abort();

    strcpy(VERSION, VER_PREFIX);
    strcat(VERSION, COMPILEDATE);
    strcat(VERSION, "-");
    strcat(VERSION, GITREV);

    printf("odroid-go-firmware (%s). HEAP=%#010x\n", VERSION, esp_get_free_heap_size());

    nvs_flash_init();

    input_init();


    // turn LED on
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 1);

    sdcardret = odroid_sdcard_open(SD_CARD); // before LCD

    ili9341_init();
    ili9341_clear(0xffff);

    UG_Init(&gui, pset, 320, 240);

    read_partition_table();
    read_app_table();

    ui_choose_app();

    indicate_error();
}
