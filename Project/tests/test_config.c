#include "fpv/config.h"
#include <stdio.h>
#include <string.h>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL config:%d %s [%s]\n", __LINE__, #x, error);                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void)
{
    fpv_config c;
    char error[256] = "";
    fpv_config_defaults(&c);
    REQUIRE(fpv_config_validate(&c, error, sizeof error) == 0);
    c.fec_k = 13;
    c.fec_n = 12;
    REQUIRE(fpv_config_validate(&c, error, sizeof error) < 0);
    fpv_config_defaults(&c);
    c.osd_enabled = 1;
    c.width = 320;
    c.height = 240;
    REQUIRE(fpv_config_validate(&c, error, sizeof error) < 0);
    fpv_config_defaults(&c);
    c.osd_enabled = 1;
    c.serial_baud = 12345;
    REQUIRE(fpv_config_validate(&c, error, sizeof error) < 0);
    fpv_config_defaults(&c);
    c.port = 0;
    REQUIRE(fpv_config_validate(&c, error, sizeof error) < 0);
    fpv_config_defaults(&c);
    strcpy(c.iface, "wlan0;touch");
    REQUIRE(fpv_config_validate(&c, error, sizeof error) < 0);
    FILE *f = fopen("test-config.ini", "w");
    REQUIRE(f);
    fputs("[system]\nrole=air\n[video]\nbackend=file\nsource=\"a "
          "path/input.h265\"\n[transport]\nport=5609\nssrc=0xF1234567\n",
          f);
    fclose(f);
    REQUIRE(fpv_config_load("test-config.ini", &c, error, sizeof error) == 0);
    REQUIRE(c.port == 5609 && c.ssrc == 0xF1234567u && !strcmp(c.source, "a path/input.h265"));
    f = fopen("test-config.ini", "w");
    REQUIRE(f);
    fputs("[video]\nframerate_typo=30\n", f);
    fclose(f);
    REQUIRE(fpv_config_load("test-config.ini", &c, error, sizeof error) < 0);
    REQUIRE(strstr(error, "framerate_typo"));
    f = fopen("test-config.ini", "w");
    REQUIRE(f);
    fputs("[transport]\nport=5600oops\n", f);
    fclose(f);
    REQUIRE(fpv_config_load("test-config.ini", &c, error, sizeof error) < 0);
    remove("test-config.ini");
    puts("PASS config: valid defaults, bounds, unsafe identifier, quoted path, u32, unknown key");
    return 0;
}
