/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/input.h>

#include "recovery_ui.h"
#include "common.h"
#include "extendedcommands.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "minini/minIni.h"

#define sizearray(a)  (sizeof(a) / sizeof((a)[0]))

char **MENU_ITEMS;

char menutitle[10][50];
char kernel[10][350];
char ramdisk[10][350];
char cmdline[10][350];
char dtb[10][350];
int is_not_empty = 1;

void set_multiboot_menu(void) {

	char section[20];
	char key[20];
	char val[350];
	char inifile_external[] = "/storage/sdcard1/bootmenu/settings.ini";
	char inifile_internal[] = "/data/media/0/bootmenu/settings.ini";
	char inifile[50];
	long n;
	int s, k;

	MENU_ITEMS = (char **)malloc(sizeof(char *) * 13 * 50);

	MENU_ITEMS[0] = (char *)malloc(sizeof(char) * 50);
	MENU_ITEMS[1] = (char *)malloc(sizeof(char) * 50);
	MENU_ITEMS[2] = (char *)malloc(sizeof(char) * 50);
	sprintf(MENU_ITEMS[0], "reboot");
	sprintf(MENU_ITEMS[1], "shutdown");
	sprintf(MENU_ITEMS[2], "patch rom package");

	if (bootmenu_internal_sd_file_exists()) {
		snprintf(inifile, sizeof(inifile), "%s", inifile_internal);
	} else {
		snprintf(inifile, sizeof(inifile), "%s", inifile_external);
	}

	for (s = 0; ini_getsection(s, section, sizearray(section), inifile) > 0 && s <= 10; s++) {
		//ui_print("    [%s]\n", section);

		for (k = 0; ini_getkey(section, k, key, sizearray(key), inifile) > 0; k++) {
			//ui_print("\t%s=", key);

			ini_gets(section, key, "", val, sizearray(val), inifile);
			//ui_print("%s\n", val);

			if (strcmp(key,"menutitle")==0) {
				sprintf(menutitle[s], "%s", val);
			} else {
				is_not_empty = 0;
			}
			if (strcmp(key,"kernel")==0) {
				sprintf(kernel[s], "%s", val);
			} else {
				is_not_empty = 0;
			}
			if (strcmp(key,"ramdisk")==0) {
				sprintf(ramdisk[s], "%s", val);
			} else {
				is_not_empty = 0;
			}
			if (strcmp(key,"cmdline")==0) {
				sprintf(cmdline[s], "%s", val);
			} else {
				is_not_empty = 0;
			}
			if (strcmp(key,"dtb")==0) {
				sprintf(dtb[s], "%s", val);
			} else {
				is_not_empty = 0;
			}
		}

		MENU_ITEMS[s + 3] = (char *)malloc(sizeof(char) * 50);
		sprintf(MENU_ITEMS[s + 3], "%s", menutitle[s]);
	}
	MENU_ITEMS[s + 3] = NULL;
}

char* MENU_HEADERS[] = { NULL };

void device_ui_init(UIParameters* ui_parameters) {
}

extern int ensure_path_mounted(const char* path);

int device_recovery_start() {
    int check_m = ensure_path_mounted("/storage/sdcard1");
    int check_mm = ensure_path_mounted("/data");
    __system("/sbin/chmod 755 /data/media/0/bootmenu/multiboot.sh");
    __system("/sbin/chmod 755 /data/media/0/bootmenu/bin/*");
    set_multiboot_menu();
    return 0;
}

int device_reboot_now(volatile char* key_pressed, int key_code) {
    return 0;
}

int device_perform_action(int which) {
    return which;
}

int device_wipe_data() {
    return 0;
}
