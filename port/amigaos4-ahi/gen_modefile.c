/*
 * gen_modefile.c - host tool that writes the BLUETOOTH AudioModes file.
 *
 * AHI finds a driver by reading DEVS:AudioModes, so a driver without an entry
 * there is on disk and invisible. The file is IFF, big-endian:
 *
 *   FORM AHIM {
 *       AUDN { "bluetooth\0" }    driver name, NUL terminated
 *       AUDM { TagItem[] ... }    one per mode
 *   }
 *
 * AUDN is what AHI turns into "DEVS:AHI/<audn>.audio" and opens; without it the
 * driver is never opened at all and the modes end up with no driver behind
 * them.
 *
 * AHIDB_Name is a relative tag: its ti_Data is an offset from the start of the
 * chunk data to the string, which AHI turns into a pointer when it reads the
 * file. The string therefore lives after the tags and the offset is the size of
 * the tag array.
 *
 * Structure and tag layout taken from the USB Audio driver's generator, which
 * is where the details of what AHI actually requires were worked out.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define AHI_TagBase      0x80000000UL
#define AHI_TagBaseR     (AHI_TagBase | 0x8000UL)

#define AHIDB_AudioID      (AHI_TagBase + 100)
#define AHIDB_Volume       (AHI_TagBase + 103)
#define AHIDB_Panning      (AHI_TagBase + 104)
#define AHIDB_Stereo       (AHI_TagBase + 105)
#define AHIDB_HiFi         (AHI_TagBase + 106)
#define AHIDB_MultTable    (AHI_TagBase + 108)
#define AHIDB_Name         (AHI_TagBaseR + 109)
#define AHIDB_MultiChannel (AHI_TagBase + 144)
#define TAG_DONE           0UL

/*
 * The mode id. The high byte identifies the driver - 0xBB for Bluetooth, since
 * 0x55 is taken by USB Audio - and the rest is the mode within it. There is one
 * mode: A2DP is stereo 16 bit at 44100 and offering variations that would be
 * silently resampled would be a promise we do not keep.
 */
#define BLUETOOTH_MODE_ID  0x00BB0001UL
#define DRIVER_NAME        "bluetooth"
#define MODE_NAME          "Bluetooth: A2DP"

static void write_be32(FILE * f, uint32_t val){
    uint8_t b[4];
    b[0] = (val >> 24) & 0xFF;
    b[1] = (val >> 16) & 0xFF;
    b[2] = (val >>  8) & 0xFF;
    b[3] = (val >>  0) & 0xFF;
    fwrite(b, 1, 4, f);
}

static void write_tag(FILE * f, uint32_t tag, uint32_t value){
    write_be32(f, tag);
    write_be32(f, value);
}

int main(void){

    const size_t drv_len     = strlen(DRIVER_NAME) + 1;          /* the NUL matters */
    const size_t drv_padded  = (drv_len + 1) & ~1u;              /* IFF pads to even */
    const size_t name_len    = strlen(MODE_NAME) + 1;
    const size_t name_padded = (name_len + 1) & ~1u;

    const uint32_t tags_bytes = 9 * 8;                            /* 8 tags + TAG_DONE */
    const uint32_t audm_size  = tags_bytes + (uint32_t) name_padded;

    /* "AHIM" + AUDN header and body + AUDM header and body */
    const uint32_t form_body = 4
                             + 8 + (uint32_t) drv_padded
                             + 8 + audm_size;

    FILE * f = fopen("BLUETOOTH", "wb");
    if (f == NULL){
        fprintf(stderr, "gen_modefile: cannot write BLUETOOTH\n");
        return 1;
    }

    fwrite("FORM", 1, 4, f);
    write_be32(f, form_body);
    fwrite("AHIM", 1, 4, f);

    fwrite("AUDN", 1, 4, f);
    write_be32(f, (uint32_t) drv_len);
    fwrite(DRIVER_NAME, 1, drv_len, f);
    if (drv_padded > drv_len) fputc(0, f);

    fwrite("AUDM", 1, 4, f);
    write_be32(f, audm_size);

    write_tag(f, AHIDB_AudioID,      BLUETOOTH_MODE_ID);
    write_tag(f, AHIDB_Volume,       1);
    write_tag(f, AHIDB_Panning,      1);
    write_tag(f, AHIDB_Stereo,       1);
    write_tag(f, AHIDB_HiFi,         1);
    write_tag(f, AHIDB_MultTable,    0);
    write_tag(f, AHIDB_MultiChannel, 0);
    write_tag(f, AHIDB_Name,         tags_bytes);   /* offset to the string below */
    write_tag(f, TAG_DONE,           0);

    fwrite(MODE_NAME, 1, name_len, f);
    if (name_padded > name_len) fputc(0, f);

    fclose(f);
    printf("BLUETOOTH written: driver '%s', mode '%s', id 0x%08lX\n",
           DRIVER_NAME, MODE_NAME, (unsigned long) BLUETOOTH_MODE_ID);
    return 0;
}
