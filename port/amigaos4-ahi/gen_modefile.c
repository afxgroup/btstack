/*
 * Writes the AHI audio mode file for bluetooth.audio.
 *
 * AHI finds drivers by reading DEVS:AudioModes: each file is an IFF AHIM chunk
 * naming a driver, an ID and how the mode should be presented. Without one the
 * driver is on disk and invisible.
 *
 * The ID's high word identifies the driver and the low word the mode within it.
 * 0xBLUE is not in use by anything else and is easy to recognise in a log.
 */

#include <stdio.h>
#include <string.h>

#define MODE_ID    0xBB1E0000u
#define MODE_NAME  "Bluetooth: A2DP headphones"
#define DRIVER     "bluetooth"

static void put_be32(FILE * f, unsigned long v){
    fputc((v >> 24) & 0xff, f);
    fputc((v >> 16) & 0xff, f);
    fputc((v >>  8) & 0xff, f);
    fputc((v      ) & 0xff, f);
}

static void put_chunk(FILE * f, const char * id, const void * data, unsigned long len){
    fwrite(id, 1, 4, f);
    put_be32(f, len);
    fwrite(data, 1, len, f);
    if (len & 1) fputc(0, f);          /* IFF chunks are word aligned */
}

int main(void){
    FILE * f = fopen("BLUETOOTH", "wb");
    if (f == NULL){
        fprintf(stderr, "cannot write the mode file\n");
        return 1;
    }

    /* FORM ... AHIM, with the length filled in once it is known */
    fwrite("FORM", 1, 4, f);
    put_be32(f, 0);
    fwrite("AHIM", 1, 4, f);

    unsigned char id[4] = { 0xBB, 0x1E, 0x00, 0x00 };
    put_chunk(f, "AHIG", id, sizeof(id));

    put_chunk(f, "NAME", MODE_NAME, strlen(MODE_NAME));
    put_chunk(f, "AUDN", DRIVER,    strlen(DRIVER));

    long end = ftell(f);
    fseek(f, 4, SEEK_SET);
    put_be32(f, (unsigned long) (end - 8));
    fclose(f);

    printf("BLUETOOTH written, %ld bytes\n", end);
    return 0;
}
